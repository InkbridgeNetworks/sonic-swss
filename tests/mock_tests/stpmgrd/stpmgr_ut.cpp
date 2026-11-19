/*
 * stpmgrd unit tests. Each test writes CONFIG_DB and STATE_DB entries in the formats of
 * sonic-spanning-tree.yang, runs StpMgr::doTask(), and decodes the datagrams that
 * stpmgrd sent towards stpd. sendto() is wrapped (-Wl,-wrap,sendto) to capture them.
 */
#include "gtest/gtest.h"
#include "../mock_table.h"
#include "stpmgr.h"

#include <cstring>
#include <deque>
#include <vector>

namespace stpmgr_ut
{
    struct SentMsg
    {
        STP_IPC_MSG header;
        std::vector<uint8_t> body;
    };

    std::vector<SentMsg> g_sent;
}

extern "C" ssize_t __wrap_sendto(int, const void *buf, size_t len, int, const struct sockaddr *, socklen_t)
{
    stpmgr_ut::SentMsg msg;
    const uint8_t *bytes = static_cast<const uint8_t *>(buf);

    memcpy(&msg.header, bytes, offsetof(STP_IPC_MSG, data));
    msg.body.assign(bytes + offsetof(STP_IPC_MSG, data), bytes + len);
    stpmgr_ut::g_sent.push_back(msg);

    return static_cast<ssize_t>(len);
}

namespace stpmgr_ut
{
    using namespace swss;
    using std::string;
    using std::vector;

    // Reaches the protected Orch::getExecutor() to queue DEL operations
    class TestStpMgr : public StpMgr
    {
    public:
        using StpMgr::StpMgr;

        void delEntry(const string &table, const string &key)
        {
            Consumer *consumer = dynamic_cast<Consumer *>(getExecutor(table));
            ASSERT_NE(consumer, nullptr);
            consumer->addToSync(KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
        }
    };

    template <typename T>
    T bodyAs(const SentMsg &msg)
    {
        T out;
        EXPECT_GE(msg.body.size(), sizeof(T));
        memcpy(&out, msg.body.data(), sizeof(T));
        return out;
    }

    vector<SentMsg> sentOfType(STP_MSG_TYPE type)
    {
        vector<SentMsg> out;
        for (const auto &msg : g_sent)
        {
            if (msg.header.msg_type == type)
            {
                out.push_back(msg);
            }
        }
        return out;
    }

    struct StpMgrTest : public ::testing::Test
    {
        std::shared_ptr<DBConnector> m_config_db;
        std::shared_ptr<DBConnector> m_app_db;
        std::shared_ptr<DBConnector> m_state_db;
        std::unique_ptr<TestStpMgr> m_stpmgr;

        void SetUp() override
        {
            testing_db::reset();
            g_sent.clear();

            m_config_db = std::make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<DBConnector>("STATE_DB", 0);

            // The tables stpmgrd.cpp subscribes to
            vector<TableConnector> tables = {
                TableConnector(m_config_db.get(), CFG_STP_GLOBAL_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_STP_VLAN_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_STP_VLAN_PORT_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_STP_PORT_TABLE_NAME),
                TableConnector(m_config_db.get(), "STP_MST"),
                TableConnector(m_config_db.get(), "STP_MST_INST"),
                TableConnector(m_config_db.get(), "STP_MST_PORT"),
                TableConnector(m_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME),
                TableConnector(m_config_db.get(), CFG_LAG_MEMBER_TABLE_NAME),
            };
            m_stpmgr = std::make_unique<TestStpMgr>(m_config_db.get(), m_app_db.get(), m_state_db.get(), tables);
        }

        void setConfig(const string &table, const string &key, const vector<FieldValueTuple> &fvs)
        {
            Table t(m_config_db.get(), table);
            t.set(key, fvs);
            m_stpmgr->addExistingData(&t);
        }

        // A VLAN member as vlanmgrd leaves it: CONFIG_DB VLAN_MEMBER plus STATE_DB VLAN_MEMBER_TABLE
        void addVlanMember(const string &vlan, const string &ifname, const string &tagging_mode, bool notify)
        {
            Table cfg(m_config_db.get(), CFG_VLAN_MEMBER_TABLE_NAME);
            cfg.set(vlan + "|" + ifname, { { "tagging_mode", tagging_mode } });
            Table state(m_state_db.get(), STATE_VLAN_MEMBER_TABLE_NAME);
            state.set(vlan + "|" + ifname, { { "state", "ok" } });
            if (notify)
            {
                m_stpmgr->addExistingData(&state);
            }
        }

        void enableMst()
        {
            setConfig(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL", { { "mode", "mst" } });
            m_stpmgr->doTask();
        }

        // Orch::doTask() runs consumers in table name order, and the MST tasks wait for STP_PORT
        void runTasks()
        {
            for (int pass = 0; pass < 3; pass++)
            {
                m_stpmgr->doTask();
            }
        }
    };

    TEST_F(StpMgrTest, MstEnableSendsBridgeConfigInMstMode)
    {
        enableMst();

        auto msgs = sentOfType(STP_BRIDGE_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        EXPECT_EQ(msgs[0].header.proto_mode, L2_MSTP);
        auto body = bodyAs<STP_BRIDGE_CONFIG_MSG>(msgs[0]);
        EXPECT_EQ(body.opcode, STP_SET_COMMAND);
        EXPECT_EQ(body.stp_mode, L2_MSTP);
    }

    TEST_F(StpMgrTest, MstDisableSendsBridgeDelInMstMode)
    {
        enableMst();
        g_sent.clear();

        m_stpmgr->delEntry(CFG_STP_GLOBAL_TABLE_NAME, "GLOBAL");
        m_stpmgr->doTask();

        auto msgs = sentOfType(STP_BRIDGE_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        EXPECT_EQ(msgs[0].header.proto_mode, L2_MSTP);
        EXPECT_EQ(bodyAs<STP_BRIDGE_CONFIG_MSG>(msgs[0]).opcode, STP_DEL_COMMAND);
    }

    TEST_F(StpMgrTest, MstEnableSendsExistingVlanMembers)
    {
        addVlanMember("Vlan10", "Ethernet0", "untagged", false);
        addVlanMember("Vlan10", "Ethernet4", "tagged", false);
        addVlanMember("Vlan20", "Ethernet4", "tagged", false);

        enableMst();

        auto msgs = sentOfType(STP_MST_VLAN_PORT_LIST_CONFIG);
        ASSERT_EQ(msgs.size(), 2u);
        for (const auto &msg : msgs)
        {
            EXPECT_EQ(msg.header.proto_mode, L2_MSTP);
        }

        auto vlan10 = bodyAs<STP_MST_VLAN_PORT_MAP>(msgs[0]);
        EXPECT_EQ(vlan10.vlan_id, 10);
        EXPECT_EQ(vlan10.add, 1);
        ASSERT_EQ(vlan10.port_count, 2);
        ASSERT_EQ(msgs[0].body.size(), sizeof(STP_MST_VLAN_PORT_MAP) + 2 * sizeof(PORT_LIST));
        PORT_LIST ports[2];
        memcpy(ports, msgs[0].body.data() + sizeof(STP_MST_VLAN_PORT_MAP), sizeof(ports));
        EXPECT_STREQ(ports[0].intf_name, "Ethernet0");
        EXPECT_EQ(ports[0].tagging_mode, UNTAGGED_MODE);
        EXPECT_STREQ(ports[1].intf_name, "Ethernet4");
        EXPECT_EQ(ports[1].tagging_mode, TAGGED_MODE);

        EXPECT_EQ(bodyAs<STP_MST_VLAN_PORT_MAP>(msgs[1]).vlan_id, 20);
    }

    TEST_F(StpMgrTest, MstVlanMemberAddAndRemove)
    {
        enableMst();
        g_sent.clear();

        addVlanMember("Vlan30", "Ethernet8", "tagged", true);
        m_stpmgr->doTask();
        m_stpmgr->delEntry(STATE_VLAN_MEMBER_TABLE_NAME, "Vlan30|Ethernet8");
        m_stpmgr->doTask();

        auto msgs = sentOfType(STP_MST_VLAN_PORT_LIST_CONFIG);
        ASSERT_EQ(msgs.size(), 2u);
        auto add = bodyAs<STP_MST_VLAN_PORT_MAP>(msgs[0]);
        EXPECT_EQ(add.vlan_id, 30);
        EXPECT_EQ(add.add, 1);
        EXPECT_EQ(add.port_count, 1);
        auto del = bodyAs<STP_MST_VLAN_PORT_MAP>(msgs[1]);
        EXPECT_EQ(del.vlan_id, 30);
        EXPECT_EQ(del.add, 0);
        EXPECT_TRUE(sentOfType(STP_VLAN_MEM_CONFIG).empty());
    }

    TEST_F(StpMgrTest, MstGlobalConfig)
    {
        enableMst();
        g_sent.clear();

        setConfig("STP_MST", "GLOBAL", { { "name", "region1" }, { "revision", "7" }, { "max_hops", "30" },
                                         { "hello_time", "3" }, { "max_age", "25" }, { "forward_delay", "20" } });
        m_stpmgr->doTask();

        auto msgs = sentOfType(STP_MST_GLOBAL_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        auto body = bodyAs<STP_MST_GLOBAL_CONFIG_MSG>(msgs[0]);
        EXPECT_EQ(body.opcode, STP_SET_COMMAND);
        EXPECT_STREQ(body.name, "region1");
        EXPECT_EQ(body.revision_number, 7u);
        EXPECT_EQ(body.max_hops, 30);
        EXPECT_EQ(body.hello_time, 3);
        EXPECT_EQ(body.max_age, 25);
        EXPECT_EQ(body.forward_delay, 20);
    }

    TEST_F(StpMgrTest, MstInstanceWithVlans)
    {
        enableMst();
        g_sent.clear();

        setConfig("STP_MST_INST", "1", { { "bridge_priority", "4096" }, { "vlan@", "10,20" } });
        runTasks();

        auto msgs = sentOfType(STP_MST_INST_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        const auto &body = msgs[0].body;
        ASSERT_EQ(body.size(), sizeof(STP_MST_INSTANCE_CONFIG_MSG) + sizeof(STP_MST_INST_CONFIG_MSG) + 2 * sizeof(VLAN_LIST));
        EXPECT_EQ(body[0], 1);  // mst_count

        STP_MST_INST_CONFIG_MSG inst;
        memcpy(&inst, body.data() + offsetof(STP_MST_INSTANCE_CONFIG_MSG, mst_list), sizeof(inst));
        EXPECT_EQ(inst.opcode, STP_SET_COMMAND);
        EXPECT_EQ(inst.mst_id, 1);
        EXPECT_EQ(inst.priority, 4096);
        ASSERT_EQ(inst.vlan_count, 2);

        VLAN_LIST vlans[2];
        memcpy(vlans, body.data() + offsetof(STP_MST_INSTANCE_CONFIG_MSG, mst_list) + sizeof(inst), sizeof(vlans));
        EXPECT_EQ(vlans[0].vlan_id, 10);
        EXPECT_EQ(vlans[1].vlan_id, 20);
    }

    TEST_F(StpMgrTest, MstInstanceWithoutVlansIsDeleted)
    {
        enableMst();
        g_sent.clear();

        setConfig("STP_MST_INST", "1", { { "bridge_priority", "4096" } });
        runTasks();

        auto msgs = sentOfType(STP_MST_INST_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        STP_MST_INST_CONFIG_MSG inst;
        memcpy(&inst, msgs[0].body.data() + offsetof(STP_MST_INSTANCE_CONFIG_MSG, mst_list), sizeof(inst));
        EXPECT_EQ(inst.opcode, STP_DEL_COMMAND);
        EXPECT_EQ(inst.mst_id, 1);
    }

    TEST_F(StpMgrTest, CistWithoutVlansKeepsPriority)
    {
        enableMst();
        g_sent.clear();

        setConfig("STP_MST_INST", "0", { { "bridge_priority", "8192" } });
        runTasks();

        auto msgs = sentOfType(STP_MST_INST_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        STP_MST_INST_CONFIG_MSG inst;
        memcpy(&inst, msgs[0].body.data() + offsetof(STP_MST_INSTANCE_CONFIG_MSG, mst_list), sizeof(inst));
        EXPECT_EQ(inst.opcode, STP_SET_COMMAND);
        EXPECT_EQ(inst.mst_id, 0);
        EXPECT_EQ(inst.priority, 8192);
    }

    TEST_F(StpMgrTest, MstInstanceDelete)
    {
        enableMst();
        setConfig("STP_MST_INST", "1", { { "vlan@", "10" } });
        runTasks();
        g_sent.clear();

        m_stpmgr->delEntry("STP_MST_INST", "1");
        runTasks();

        auto msgs = sentOfType(STP_MST_INST_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        STP_MST_INST_CONFIG_MSG inst;
        memcpy(&inst, msgs[0].body.data() + offsetof(STP_MST_INSTANCE_CONFIG_MSG, mst_list), sizeof(inst));
        EXPECT_EQ(inst.opcode, STP_DEL_COMMAND);
        EXPECT_EQ(inst.mst_id, 1);
    }

    TEST_F(StpMgrTest, MstInstanceRejectsNonYangKeysAndValues)
    {
        enableMst();
        g_sent.clear();

        setConfig("STP_MST_INST", "MST_INSTANCE|1", { { "vlan@", "10" } });
        setConfig("STP_MST_INST", "MST_INSTANCE:INSTANCE0", { { "bridge_priority", "4096" } });
        setConfig("STP_MST_INST", "2", { { "vlan@", "4095" } });
        setConfig("STP_MST_INST", "3", { { "vlan@", "Vlan10" } });
        runTasks();

        EXPECT_TRUE(sentOfType(STP_MST_INST_CONFIG).empty());
    }

    TEST_F(StpMgrTest, MstPortConfig)
    {
        enableMst();
        g_sent.clear();

        setConfig(CFG_STP_PORT_TABLE_NAME, "Ethernet0",
                  { { "enabled", "true" }, { "edge_port", "true" }, { "link_type", "point-to-point" },
                    { "bpdu_guard", "true" }, { "bpdu_guard_do_disable", "true" }, { "root_guard", "true" },
                    { "priority", "64" }, { "path_cost", "2000" } });
        m_stpmgr->doTask();

        auto msgs = sentOfType(STP_PORT_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        EXPECT_EQ(msgs[0].header.proto_mode, L2_MSTP);
        auto body = bodyAs<STP_PORT_CONFIG_MSG>(msgs[0]);
        EXPECT_EQ(body.opcode, STP_SET_COMMAND);
        EXPECT_STREQ(body.intf_name, "Ethernet0");
        EXPECT_EQ(body.enabled, 1);
        EXPECT_EQ(body.edge_port, 1);
        EXPECT_EQ(body.link_type, POINT_TO_POINT);
        EXPECT_EQ(body.bpdu_guard, 1);
        EXPECT_EQ(body.bpdu_guard_do_disable, 1);
        EXPECT_EQ(body.root_guard, 1);
        EXPECT_EQ(body.priority, 64);
        EXPECT_EQ(body.path_cost, 2000);
    }

    TEST_F(StpMgrTest, MstPortDefaults)
    {
        enableMst();
        g_sent.clear();

        setConfig(CFG_STP_PORT_TABLE_NAME, "Ethernet0", { { "enabled", "true" } });
        m_stpmgr->doTask();

        auto msgs = sentOfType(STP_PORT_CONFIG);
        ASSERT_EQ(msgs.size(), 1u);
        auto body = bodyAs<STP_PORT_CONFIG_MSG>(msgs[0]);
        // mstpmgr_process_intf_config_msg(): priority -1 and path_cost 0 select the defaults
        EXPECT_EQ(body.priority, -1);
        EXPECT_EQ(body.path_cost, 0);
        EXPECT_EQ(body.link_type, AUTO);
    }

    TEST_F(StpMgrTest, MstPortLinkTypes)
    {
        const vector<std::pair<string, LinkType>> cases = {
            { "auto", AUTO }, { "shared", SHARED }, { "point-to-point", POINT_TO_POINT },
        };

        enableMst();
        for (const auto &c : cases)
        {
            g_sent.clear();
            setConfig(CFG_STP_PORT_TABLE_NAME, "Ethernet0", { { "enabled", "true" }, { "link_type", c.first } });
            m_stpmgr->doTask();

            auto msgs = sentOfType(STP_PORT_CONFIG);
            ASSERT_EQ(msgs.size(), 1u) << c.first;
            EXPECT_EQ(bodyAs<STP_PORT_CONFIG_MSG>(msgs[0]).link_type, c.second) << c.first;
        }

        for (const string bad : { "p2p", "1", "Auto" })
        {
            g_sent.clear();
            setConfig(CFG_STP_PORT_TABLE_NAME, "Ethernet0", { { "enabled", "true" }, { "link_type", bad } });
            m_stpmgr->doTask();
            EXPECT_TRUE(sentOfType(STP_PORT_CONFIG).empty()) << bad;
        }
    }

    TEST_F(StpMgrTest, MstInstancePortConfig)
    {
        enableMst();
        setConfig(CFG_STP_PORT_TABLE_NAME, "Ethernet0", { { "enabled", "true" } });
        setConfig("STP_MST_INST", "1", { { "vlan@", "10" } });
        runTasks();
        g_sent.clear();

        setConfig("STP_MST_PORT", "1|Ethernet0", { { "priority", "32" } });
        setConfig("STP_MST_PORT", "1|Ethernet4", { { "path_cost", "500" }, { "priority", "16" } });
        runTasks();

        auto msgs = sentOfType(STP_MST_INST_PORT_CONFIG);
        ASSERT_EQ(msgs.size(), 2u);
        auto prio_only = bodyAs<STP_MST_INST_PORT_CONFIG_MSG>(msgs[0]);
        EXPECT_EQ(prio_only.opcode, STP_SET_COMMAND);
        EXPECT_STREQ(prio_only.intf_name, "Ethernet0");
        EXPECT_EQ(prio_only.mst_id, 1);
        EXPECT_EQ(prio_only.priority, 32);
        EXPECT_EQ(prio_only.path_cost, -1);
        auto both = bodyAs<STP_MST_INST_PORT_CONFIG_MSG>(msgs[1]);
        EXPECT_STREQ(both.intf_name, "Ethernet4");
        EXPECT_EQ(both.path_cost, 500);
        EXPECT_EQ(both.priority, 16);
    }

    TEST_F(StpMgrTest, MstInstancePortRejectsNonYangKeys)
    {
        enableMst();
        setConfig(CFG_STP_PORT_TABLE_NAME, "Ethernet0", { { "enabled", "true" } });
        setConfig("STP_MST_INST", "1", { { "vlan@", "10" } });
        runTasks();
        g_sent.clear();

        setConfig("STP_MST_PORT", "MST_INSTANCE|1|Ethernet0", { { "priority", "32" } });
        setConfig("STP_MST_PORT", "1", { { "priority", "32" } });
        runTasks();

        EXPECT_TRUE(sentOfType(STP_MST_INST_PORT_CONFIG).empty());
    }
}
