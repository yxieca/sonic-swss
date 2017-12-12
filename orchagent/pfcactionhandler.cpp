#include "pfcactionhandler.h"
#include "logger.h"
#include "sai_serialize.h"
#include "portsorch.h"
#include <vector>

#define PFC_WD_QUEUE_STATUS             "PFC_WD_STATUS"
#define PFC_WD_QUEUE_STATUS_OPERATIONAL "operational"
#define PFC_WD_QUEUE_STATUS_STORMED     "stormed"

#define PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED "PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED"
#define PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED"

#define SAI_QUEUE_STAT_PACKETS_STR         "SAI_QUEUE_STAT_PACKETS"
#define SAI_QUEUE_STAT_DROPPED_PACKETS_STR "SAI_QUEUE_STAT_DROPPED_PACKETS"

extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;
extern AclOrch * gAclOrch;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_buffer_api_t *sai_buffer_api;

PfcWdActionHandler::PfcWdActionHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    m_port(port),
    m_queue(queue),
    m_queueId(queueId),
    m_countersTable(countersTable)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE(
            "PFC Watchdog detected PFC storm on queue 0x%lx port 0x%lx",
            m_queue,
            m_port);

    m_stats = getQueueStats(m_countersTable, sai_serialize_object_id(m_queue));
    m_stats.detectCount++;
    m_stats.operational = false;

    updateWdCounters(sai_serialize_object_id(m_queue), m_stats);
}

PfcWdActionHandler::~PfcWdActionHandler(void)
{
    SWSS_LOG_ENTER();

    auto finalStats = getQueueStats(m_countersTable, sai_serialize_object_id(m_queue));

    SWSS_LOG_NOTICE(
            "Queue 0x%lx port 0x%lx restored from PFC storm. Tx packets: %lu. Dropped packets: %lu",
            m_queue,
            m_port,
            finalStats.txPkt - m_stats.txPkt,
            finalStats.txDropPkt - m_stats.txDropPkt);

    finalStats.restoreCount++;
    finalStats.operational = true;

    updateWdCounters(sai_serialize_object_id(m_queue), finalStats);
}

PfcWdActionHandler::PfcWdQueueStats PfcWdActionHandler::getQueueStats(shared_ptr<Table> countersTable, const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    PfcWdQueueStats stats;
    vector<FieldValueTuple> fieldValues;

    if (!countersTable->get(queueIdStr, fieldValues))
    {
        return move(stats);
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED)
        {
            stats.detectCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED)
        {
            stats.restoreCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATUS)
        {
            stats.operational = value == PFC_WD_QUEUE_STATUS_OPERATIONAL ? true : false;
        }
        else if (field == SAI_QUEUE_STAT_PACKETS_STR)
        {
            stats.txPkt = stoul(value);
        }
        else if (field == SAI_QUEUE_STAT_DROPPED_PACKETS_STR)
        {
            stats.txDropPkt = stoul(value);
        }
    }

    return move(stats);
}

void PfcWdActionHandler::initWdCounters(shared_ptr<Table> countersTable, const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    auto stats = getQueueStats(countersTable, queueIdStr);

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, PFC_WD_QUEUE_STATUS_OPERATIONAL);

    countersTable->set(queueIdStr, resultFvValues);
}

void PfcWdActionHandler::updateWdCounters(const string& queueIdStr, const PfcWdQueueStats& stats)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, stats.operational ?
                                                     PFC_WD_QUEUE_STATUS_OPERATIONAL :
                                                     PFC_WD_QUEUE_STATUS_STORMED);

    m_countersTable->set(queueIdStr, resultFvValues);
}

PfcWdAclHandler::PfcWdAclHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdActionHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    acl_table_type_t table_type = ACL_TABLE_L3;

    // There is one handler instance per queue ID
    string queuestr = to_string(queueId);
    m_strIngressTable = "IngressTable_PfcWdAclHandler_" + queuestr;
    m_strEgressTable = "EgressTable_PfcWdAclHandler_" + queuestr;
    m_strRule = "Rule_PfcWdAclHandler_" + queuestr;

    auto found = m_aclTables.find(m_strIngressTable);
    if (found == m_aclTables.end())
    {
        // First time of handling PFC for this queue, create ACL table, and bind
        createPfcAclTable(port, m_strIngressTable, true);
        shared_ptr<AclRuleL3> newRule = make_shared<AclRuleL3>(gAclOrch, m_strRule, m_strIngressTable, table_type);
        createPfcAclRule(newRule, queueId, m_strIngressTable);
    }
    else
    {
        // Otherwise just bind ACL table with the port
        found->second.bind(port);
    }

    found = m_aclTables.find(m_strEgressTable);
    if (found == m_aclTables.end())
    {
        // First time of handling PFC for this queue, create ACL table, and bind
        createPfcAclTable(port, m_strEgressTable, false);
        shared_ptr<AclRuleL3> newRule = make_shared<AclRuleL3>(gAclOrch, m_strRule, m_strEgressTable, table_type);
        createPfcAclRule(newRule, queueId, m_strEgressTable);
    }
    else
    {
        // Otherwise just bind ACL table with the port
        found->second.bind(port);
    }
}

PfcWdAclHandler::~PfcWdAclHandler(void)
{
    SWSS_LOG_ENTER();

    auto found = m_aclTables.find(m_strIngressTable);
    found->second.unbind(getPort());

    found = m_aclTables.find(m_strEgressTable);
    found->second.unbind(getPort());
}

void PfcWdAclHandler::clear()
{
    SWSS_LOG_ENTER();

    for (auto& tablepair: m_aclTables)
    {
        auto& table = tablepair.second;
        gAclOrch->removeAclTable(table.getId());
    }
}

void PfcWdAclHandler::createPfcAclTable(sai_object_id_t port, string strTable, bool ingress)
{
    SWSS_LOG_ENTER();

    auto inserted = m_aclTables.emplace(piecewise_construct,
        std::forward_as_tuple(strTable),
        std::forward_as_tuple());

    assert(inserted.second);

    AclTable& aclTable = inserted.first->second;
    aclTable.type = ACL_TABLE_L3;
    aclTable.link(port);
    aclTable.id = strTable;
    aclTable.stage = ingress ? ACL_STAGE_INGRESS : ACL_STAGE_EGRESS;
    gAclOrch->addAclTable(aclTable, strTable);
}

void PfcWdAclHandler::createPfcAclRule(shared_ptr<AclRuleL3> rule, uint8_t queueId, string strTable)
{
    SWSS_LOG_ENTER();

    string attr_name, attr_value;

    attr_name = RULE_PRIORITY;
    attr_value = "999";
    rule->validateAddPriority(attr_name, attr_value);

    attr_name = MATCH_TC;
    attr_value = to_string(queueId);
    rule->validateAddMatch(attr_name, attr_value);

    attr_name = ACTION_PACKET_ACTION;
    attr_value = PACKET_ACTION_DROP;
    rule->validateAddAction(attr_name, attr_value);

    gAclOrch->addAclRule(rule, strTable);
}

std::map<std::string, AclTable> PfcWdAclHandler::m_aclTables;

PfcWdLossyHandler::PfcWdLossyHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdActionHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;

    sai_status_t status = sai_port_api->get_port_attribute(port, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%lx: %d", port, status);
    }

    uint8_t pfcMask = attr.value.u8;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;
    attr.value.u8 = static_cast<uint8_t>(pfcMask & ~(1 << queueId));

    status = sai_port_api->set_port_attribute(port, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%lx: %d", port, status);
    }
}

PfcWdLossyHandler::~PfcWdLossyHandler(void)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;

    sai_status_t status = sai_port_api->get_port_attribute(getPort(), 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%lx: %d", getPort(), status);
        return;
    }

    uint8_t pfcMask = attr.value.u8;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;
    attr.value.u8 = static_cast<uint8_t>(pfcMask | (1 << getQueueId()));

    status = sai_port_api->set_port_attribute(getPort(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PFC mask on port 0x%lx: %d", getPort(), status);
        return;
    }
}

PfcWdZeroBufferHandler::PfcWdZeroBufferHandler(sai_object_id_t port,
        sai_object_id_t queue, uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdLossyHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;

    // Get queue's buffer profile ID
    sai_status_t status = sai_queue_api->get_queue_attribute(queue, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get buffer profile ID on queue 0x%lx: %d", queue, status);
        return;
    }

    sai_object_id_t oldQueueProfileId = attr.value.oid;

    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = ZeroBufferProfile::getZeroBufferProfile(false);

    // Set our zero buffer profile
    status = sai_queue_api->set_queue_attribute(queue, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%lx: %d", queue, status);
        return;
    }

    // Save original buffer profile
    m_originalQueueBufferProfile = oldQueueProfileId;

    // Get PG
    Port portInstance;
    if (!gPortsOrch->getPort(port, portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%lx", port);
        return;
    }

    sai_object_id_t pg = portInstance.m_priority_group_ids[queueId];

    attr.id = SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE;

    // Get PG's buffer profile
    status = sai_buffer_api->get_ingress_priority_group_attribute(pg, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get buffer profile ID on PG 0x%lx: %d", pg, status);
        return;
    }

    // Set zero profile to PG
    sai_object_id_t oldPgProfileId = attr.value.oid;

    attr.id = SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE;
    attr.value.oid = ZeroBufferProfile::getZeroBufferProfile(true);

    status = sai_buffer_api->set_ingress_priority_group_attribute(pg, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on pg 0x%lx: %d", pg, status);
        return;
    }

    // Save original buffer profile
    m_originalPgBufferProfile = oldPgProfileId;
}

PfcWdZeroBufferHandler::~PfcWdZeroBufferHandler(void)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = m_originalQueueBufferProfile;

    // Set our zero buffer profile on a queue
    sai_status_t status = sai_queue_api->set_queue_attribute(getQueue(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%lx: %d", getQueue(), status);
        return;
    }

    Port portInstance;
    if (!gPortsOrch->getPort(getPort(), portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%lx", getPort());
        return;
    }

    sai_object_id_t pg = portInstance.m_priority_group_ids[getQueueId()];

    attr.id = SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE;
    attr.value.oid = m_originalPgBufferProfile;

    // Set our zero buffer profile
    status = sai_buffer_api->set_ingress_priority_group_attribute(pg, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%lx: %d", getQueue(), status);
        return;
    }
}

PfcWdZeroBufferHandler::ZeroBufferProfile::ZeroBufferProfile(void)
{
    SWSS_LOG_ENTER();
}

PfcWdZeroBufferHandler::ZeroBufferProfile::~ZeroBufferProfile(void)
{
    SWSS_LOG_ENTER();

    // Destory ingress and egress prifiles and pools
    destroyZeroBufferProfile(true);
    destroyZeroBufferProfile(false);
}

PfcWdZeroBufferHandler::ZeroBufferProfile &PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance(void)
{
    SWSS_LOG_ENTER();

    static ZeroBufferProfile instance;

    return instance;
}

sai_object_id_t PfcWdZeroBufferHandler::ZeroBufferProfile::getZeroBufferProfile(bool ingress)
{
    SWSS_LOG_ENTER();

    if (getInstance().getProfile(ingress) == SAI_NULL_OBJECT_ID)
    {
        getInstance().createZeroBufferProfile(ingress);
    }

    return getInstance().getProfile(ingress);
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::createZeroBufferProfile(bool ingress)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attribs;

    // Create zero pool
    attr.id = SAI_BUFFER_POOL_ATTR_SIZE;
    attr.value.u32 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_TYPE;
    attr.value.u32 = ingress ? SAI_BUFFER_POOL_TYPE_INGRESS : SAI_BUFFER_POOL_TYPE_EGRESS;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_THRESHOLD_MODE;
    attr.value.u32 = SAI_BUFFER_POOL_THRESHOLD_MODE_DYNAMIC;
    attribs.push_back(attr);

    sai_status_t status = sai_buffer_api->create_buffer_pool(
            &getPool(ingress),
            gSwitchId,
            static_cast<uint32_t>(attribs.size()),
            attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create dynamic zero buffer pool for PFC WD: %d", status);
        return;
    }

    // Create zero profile
    attribs.clear();

    attr.id = SAI_BUFFER_PROFILE_ATTR_POOL_ID;
    attr.value.oid = getPool(ingress);
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;
    attr.value.u32 = SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
    attr.value.u32 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
    attr.value.u32 = 1;
    attribs.push_back(attr);

    status = sai_buffer_api->create_buffer_profile(
            &getProfile(ingress),
            gSwitchId,
            static_cast<uint32_t>(attribs.size()),
            attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create dynamic zero buffer profile for PFC WD: %d", status);
        return;
    }
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::destroyZeroBufferProfile(bool ingress)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_buffer_api->remove_buffer_profile(getProfile(ingress));
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove static zero buffer profile for PFC WD: %d", status);
        return;
    }

    status = sai_buffer_api->remove_buffer_pool(getPool(ingress));
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove static zero buffer pool for PFC WD: %d", status);
    }
}
