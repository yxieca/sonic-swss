#include <assert.h>
#include <iostream>
#include <vector>

#include "logger.h"
#include "tokenize.h"
#include "fdborch.h"

extern sai_fdb_api_t    *sai_fdb_api;

extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;

void FdbOrch::update(sai_fdb_event_t type, const sai_fdb_entry_t* entry, sai_object_id_t bridge_port_id)
{
    SWSS_LOG_ENTER();

    FdbUpdate update;
    update.entry.mac = entry->mac_address;
    update.entry.bv_id = entry->bv_id;

    switch (type)
    {
    case SAI_FDB_EVENT_LEARNED:
        if (!m_portsOrch->getPortByBridgePortId(bridge_port_id, update.port))
        {
            SWSS_LOG_ERROR("Failed to get port by bridge port ID %lu", bridge_port_id);
            return;
        }

        update.add = true;

        (void)m_entries.insert(update.entry);
        SWSS_LOG_DEBUG("FdbOrch notification: mac %s was inserted into bv_id 0x%lx", update.entry.mac.to_string().c_str(), entry->bv_id);
        break;
    case SAI_FDB_EVENT_AGED:
    case SAI_FDB_EVENT_FLUSHED:
    case SAI_FDB_EVENT_MOVE:
        update.add = false;

        (void)m_entries.erase(update.entry);
        SWSS_LOG_DEBUG("FdbOrch notification: mac %s was removed from bv_id 0x%lx", update.entry.mac.to_string().c_str(), entry->bv_id);
        break;
    }

    for (auto observer: m_observers)
    {
        observer->update(SUBJECT_TYPE_FDB_CHANGE, static_cast<void *>(&update));
    }
}

bool FdbOrch::getPort(const MacAddress& mac, uint16_t vlan, Port& port)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch->getVlanByVlanId(vlan, port))
    {
        SWSS_LOG_ERROR("Failed to get vlan by vlan ID %d", vlan);
        return false;
    }

    sai_fdb_entry_t entry;
    memcpy(entry.mac_address, mac.getMac(), sizeof(sai_mac_t));
    entry.bv_id = port.m_vlan_info.vlan_oid;

    sai_attribute_t attr;
    attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;

    sai_status_t status = sai_fdb_api->get_fdb_entry_attribute(&entry, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get bridge port ID for FDB entry %s, rv:%d",
            mac.to_string().c_str(), status);
        return false;
    }

    if (!m_portsOrch->getPortByBridgePortId(attr.value.oid, port))
    {
        SWSS_LOG_ERROR("Failed to get port by bridge port ID %lu", attr.value.oid);
        return false;
    }

    return true;
}

void FdbOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isInitDone())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        /* format: <VLAN_name>:<MAC_address> */
        vector<string> keys = tokenize(kfvKey(t), ':', 1);
        string op = kfvOp(t);

        Port vlan;
        if (!m_portsOrch->getPort(keys[0], vlan))
        {
            SWSS_LOG_INFO("Failed to locate %s", keys[0].c_str());
            it++;
            continue;
        }

        FdbEntry entry;
        entry.mac = MacAddress(keys[1]);
        entry.bv_id = vlan.m_vlan_info.vlan_oid;

        if (op == SET_COMMAND)
        {
            string port;
            string type;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "port")
                {
                    port = fvValue(i);
                }

                if (fvField(i) == "type")
                {
                    type = fvValue(i);
                }
            }

            /* FDB type is either dynamic or static */
            assert(type == "dynamic" || type == "static");

            if (addFdbEntry(entry, port, type))
                it = consumer.m_toSync.erase(it);
            else
                it++;

            /* Remove corresponding APP_DB entry if type is 'dynamic' */
            // FIXME: The modification of table is not thread safe.
            // Uncomment this after this issue is fixed.
            // if (type == "dynamic")
            // {
            //     m_table.del(kfvKey(t));
            // }
        }
        else if (op == DEL_COMMAND)
        {
            if (removeFdbEntry(entry))
                it = consumer.m_toSync.erase(it);
            else
                it++;

        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool FdbOrch::addFdbEntry(const FdbEntry& entry, const string& port_name, const string& type)
{
    SWSS_LOG_ENTER();

    if (m_entries.count(entry) != 0) // we already have such entries
    {
        // FIXME: should we check that the entry are moving to another port?
        // FIXME: should we check that the entry are changing its type?
        SWSS_LOG_ERROR("FDB entry already exists. mac=%s bv_id=0x%lx", entry.mac.to_string().c_str(), entry.bv_id);
        return true;
    }

    sai_fdb_entry_t fdb_entry;

    fdb_entry.switch_id = gSwitchId;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.bv_id = entry.bv_id;

    Port port;
    /* Retry until port is created */
    if (!m_portsOrch->getPort(port_name, port))
    {
        SWSS_LOG_INFO("Failed to locate port %s", port_name.c_str());
        return false;
    }

    /* Retry until port is added to the VLAN */
    if (!port.m_bridge_port_id)
    {
        SWSS_LOG_INFO("Port %s does not have a bridge port ID", port_name.c_str());
        return false;
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_FDB_ENTRY_ATTR_TYPE;
    attr.value.s32 = (type == "dynamic") ? SAI_FDB_ENTRY_TYPE_DYNAMIC : SAI_FDB_ENTRY_TYPE_STATIC;
    attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
    attr.value.oid = port.m_bridge_port_id;
    attrs.push_back(attr);

    attr.id = SAI_FDB_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    sai_status_t status = sai_fdb_api->create_fdb_entry(&fdb_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create %s FDB %s on %s, rv:%d",
                type.c_str(), entry.mac.to_string().c_str(), port_name.c_str(), status);
        return false;
    }

    SWSS_LOG_NOTICE("Create %s FDB %s on %s", type.c_str(), entry.mac.to_string().c_str(), port_name.c_str());

    (void) m_entries.insert(entry);

    return true;
}

bool FdbOrch::removeFdbEntry(const FdbEntry& entry)
{
    SWSS_LOG_ENTER();

    if (m_entries.count(entry) == 0)
    {
        SWSS_LOG_ERROR("FDB entry isn't found. mac=%s bv_id=0x%lx", entry.mac.to_string().c_str(), entry.bv_id);
        return true;
    }

    sai_status_t status;
    sai_fdb_entry_t fdb_entry;
    memcpy(fdb_entry.mac_address, entry.mac.getMac(), sizeof(sai_mac_t));
    fdb_entry.bv_id = entry.bv_id;

    status = sai_fdb_api->remove_fdb_entry(&fdb_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove FDB entry. mac=%s, bv_id=0x%lx",
                       entry.mac.to_string().c_str(), entry.bv_id);
        return true;
    }

    (void)m_entries.erase(entry);

    return true;
}
