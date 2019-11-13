/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <signal.h>
#include "dbconnector.h"
#include "select.h"
#include "exec.h"
#include "schema.h"
#include "macaddress.h"
#include "producerstatetable.h"
#include "notificationproducer.h"
#include "natmgr.h"
#include "shellcmd.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

/*
 * Following global variables are defined here for the purpose of
 * using existing Orch class which is to be refactored soon to
 * eliminate the direct exposure of the global variables.
 *
 * Once Orch class refactoring is done, these global variables
 * should be removed from here.
 */
int       gBatchSize = 0;
bool      gSwssRecord = false;
bool      gLogRotate = false;
ofstream  gRecordOfs;
string    gRecordFile;
mutex     gDbMutex;
NatMgr    *natmgr = NULL;

std::shared_ptr<swss::NotificationProducer> cleanupNotifier;

void sigterm_handler(int signo)
{
    int ret = 0;
    std::string res;
    const std::string iptablesFlushNat          = "iptables -t nat -F";
    const std::string iptablesFlushMangle       = "iptables -t mangle -F";
    const std::string conntrackFlush            = "conntrack -F";

    SWSS_LOG_NOTICE("Got SIGTERM");

    /*If there are any iptables and conntrack entries, clean them */
    ret = swss::exec(iptablesFlushNat, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", iptablesFlushNat.c_str(), ret);
    }
    ret = swss::exec(iptablesFlushMangle, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", iptablesFlushMangle.c_str(), ret);
    }
    ret = swss::exec(conntrackFlush, res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", conntrackFlush.c_str(), ret);
    }

    /* Send notification to Orchagent to clean up the REDIS and ASIC database */
    if (cleanupNotifier != NULL)
    {
        SWSS_LOG_NOTICE("Sending notification to orchagent to cleanup NAT entries in REDIS/ASIC");

        std::vector<swss::FieldValueTuple> entry;

        cleanupNotifier->send("nat_cleanup", "all", entry);
    }
    
    if (natmgr)
    {
        natmgr->cleanupPoolIpTable();
    }
}

int main(int argc, char **argv)
{
    Logger::linkToDbNative("natmgrd");
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("--- Starting natmgrd ---");

    try
    {
        vector<string> cfg_tables = {
            CFG_STATIC_NAT_TABLE_NAME,
            CFG_STATIC_NAPT_TABLE_NAME,
            CFG_NAT_POOL_TABLE_NAME,
            CFG_NAT_BINDINGS_TABLE_NAME,
            CFG_NAT_GLOBAL_TABLE_NAME,
            CFG_INTF_TABLE_NAME,
            CFG_LAG_INTF_TABLE_NAME,
            CFG_VLAN_INTF_TABLE_NAME,
            CFG_LOOPBACK_INTERFACE_TABLE_NAME,
            CFG_ACL_TABLE_TABLE_NAME,
            CFG_ACL_RULE_TABLE_NAME
        };

        DBConnector cfgDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
        DBConnector stateDb(STATE_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

        cleanupNotifier = std::make_shared<swss::NotificationProducer>(&appDb, "NAT_DB_CLEANUP_NOTIFICATION");

        if (signal(SIGTERM, sigterm_handler) == SIG_ERR)
        {
            SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
            exit(1);
        }

        natmgr = new NatMgr(&cfgDb, &appDb, &stateDb, cfg_tables);
        
        std::vector<Orch *> cfgOrchList = {natmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        SWSS_LOG_NOTICE("starting main loop");
        while (true)
        {
            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                natmgr->doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}

