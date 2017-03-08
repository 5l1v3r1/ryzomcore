// Ryzom - MMORPG Framework <http://dev.ryzom.com/projects/ryzom/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#include "item_group_manager.h"
#include "interface_v3/inventory_manager.h"
#include "nel/gui/widget_manager.h"
#include "nel/misc/sheet_id.h"
#include "nel/misc/stream.h"
#include "nel/misc/o_xml.h"
#include "nel/misc/i_xml.h"
#include "nel/misc/file.h"
#include "libxml/tree.h"
#include "game_share/item_type.h"
#include "client_sheets/item_sheet.h"
#include "net_manager.h"
#include "connection.h" // Used to access PlayerSelectedFileName for xml filename
#include "nel/gui/db_manager.h"

CItemGroupManager *CItemGroupManager::_Instance = NULL;

CItemGroup::CItemGroup()
{
}


bool CItemGroup::contains(CDBCtrlSheet *other)
{
	for(auto &item : _Items)
	{
		NLMISC::CSheetId sheet = NLMISC::CSheetId(other->getSheetId());
		if (sheet.toString()  == item.sheetName  && other->getQuality()   == item.quality &&
				other->getItemWeight() == item.weight     && other->getItemColor() == item.color &&
				(!item.usePrice || (other->getItemPrice()  >= item.minPrice && other->getItemPrice() <= item.maxPrice))
				)
		{
			return true;
		}
	}

	return false;
}

void CItemGroup::addItem(std::string sheetName, uint16 quality, uint32 weight, uint8 color)
{
	_Items.push_back(CItem(sheetName, quality, weight, color));
}

void CItemGroup::writeTo(xmlNodePtr node)
{
	xmlNodePtr groupNode = xmlNewChild (node, NULL, (const xmlChar*)"group", NULL );
	xmlSetProp(groupNode, (const xmlChar*)"name", (const xmlChar*)name.c_str());
	for(auto &item: _Items)
	{
		xmlNodePtr itemNode = xmlNewChild(groupNode, NULL, (const xmlChar*)"item", NULL);
		xmlSetProp (itemNode, (const xmlChar*)"sheetName", (const xmlChar*)item.sheetName.c_str());
		xmlSetProp (itemNode, (const xmlChar*)"quality", (const xmlChar*)NLMISC::toString(item.quality).c_str());
		xmlSetProp (itemNode, (const xmlChar*)"weight", (const xmlChar*)NLMISC::toString(item.weight).c_str());
		xmlSetProp (itemNode, (const xmlChar*)"color", (const xmlChar*)NLMISC::toString(item.color).c_str());
		xmlSetProp (itemNode, (const xmlChar*)"minPrice", (const xmlChar*)NLMISC::toString(item.minPrice).c_str());
		xmlSetProp (itemNode, (const xmlChar*)"maxPrice", (const xmlChar*)NLMISC::toString(item.maxPrice).c_str());
		xmlSetProp (itemNode, (const xmlChar*)"usePrice", (const xmlChar*)NLMISC::toString(item.usePrice).c_str());


	}
}


void CItemGroup::readFrom(xmlNodePtr node)
{
	CXMLAutoPtr ptrName;
	ptrName = (char*) xmlGetProp( node, (xmlChar*)"name" );
	if (ptrName) NLMISC::fromString((const char*)ptrName, name);

	xmlNodePtr curNode = node->children;
	while(curNode)
	{
		if (NLMISC::stricmp((char*)curNode->name, "item") == 0)
		{

			CItem item;
			ptrName = (char*) xmlGetProp(curNode, (xmlChar*)"sheetName");
			if (ptrName) NLMISC::fromString((const char*)ptrName, item.sheetName);
			ptrName = (char*) xmlGetProp(curNode, (xmlChar*)"quality");
			if (ptrName) NLMISC::fromString((const char*)ptrName, item.quality);
			ptrName = (char*) xmlGetProp(curNode, (xmlChar*)"weight");
			if (ptrName) NLMISC::fromString((const char*)ptrName, item.weight);
			ptrName = (char*) xmlGetProp(curNode, (xmlChar*)"color");
			if (ptrName) NLMISC::fromString((const char*)ptrName, item.color);
			ptrName = (char*) xmlGetProp(curNode, (xmlChar*)"minPrice");
			if (ptrName) NLMISC::fromString((const char*)ptrName, item.minPrice);
			ptrName = (char*) xmlGetProp(curNode, (xmlChar*)"maxPrice");
			if (ptrName) NLMISC::fromString((const char*)ptrName, item.maxPrice);
			ptrName = (char*) xmlGetProp(curNode, (xmlChar*)"usePrice");
			if (ptrName) NLMISC::fromString((const char*)ptrName, item.usePrice);

			_Items.push_back(item);
		}
		curNode = curNode->next;
	}

}

void CFakeEquipTime::invalidActions()
{
	NLGUI::CDBManager *pDB = NLGUI::CDBManager::getInstance();
	NLMISC::CCDBNodeLeaf *node;
	// This are the db update server sends when an user equip an item, see egs/player_manager/gear_latency.cpp CGearLatency::setSlot
	node = pDB->getDbProp("SERVER:USER:ACT_TSTART", false);
	if (node) node->setValue64(NetMngr.getCurrentServerTick());

	node = pDB->getDbProp("SERVER:USER:ACT_TEND", false);
	if(node) node->setValue64(NetMngr.getCurrentServerTick() + time);

	node = pDB->getDbProp("SERVER:EXECUTE_PHRASE:SHEET", false);
	static NLMISC::CSheetId equipSheet("big_equip_item.sbrick");
	if(node) node->setValue64((sint64)equipSheet.asInt());


	node = pDB->getDbProp("SERVER:EXECUTE_PHRASE:PHRASE", false);
	if(node) node->setValue64(0);

}

void CFakeEquipTime::validActions()
{
	NLGUI::CDBManager *pDB = NLGUI::CDBManager::getInstance();
	NLMISC::CCDBNodeLeaf *node;
	node = pDB->getDbProp("SERVER:USER:ACT_TSTART", false);
	if (node) node->setValue64(0);

	node = pDB->getDbProp("SERVER:USER:ACT_TEND", false);
	if(node) node->setValue64(0);

	node = pDB->getDbProp("SERVER:EXECUTE_PHRASE:SHEET", false);
	if(node) node->setValue32(0);

	node = pDB->getDbProp("SERVER:EXECUTE_PHRASE:PHRASE", false);
	if(node) node->setValue32(0);
}
void CFakeEquipTime::run()
{
	//We wait a bit before invalidating actions, or server will override us
	//Might not be accurate for everyone, but if it's wrong at worst you'll still get the timer
	// Just with a blank icon instead of a "equipping item" red cross
	NLMISC::nlSleep(600);
	invalidActions();
	NLMISC::nlSleep((time-6) * 100); // time is in ticks, sleep takes ms
	validActions();
}

CItemGroupManager::CItemGroupManager()
{
}

void CItemGroupManager::init()
{
	loadGroups();
}

void CItemGroupManager::uninit()
{
	saveGroups();

}

// Inspired from macro parsing
void CItemGroupManager::saveGroups()
{
	std::string userGroupFileName = "save/groups_" + PlayerSelectedFileName + ".xml";
	try {
		NLMISC::COFile f;
		if(f.open(userGroupFileName, false, false, true))
		{

			NLMISC::COXml xmlStream;
			xmlStream.init(&f);
			xmlDocPtr doc = xmlStream.getDocument ();
			xmlNodePtr node = xmlNewDocNode(doc, NULL, (const xmlChar*)"item_groups", NULL);
			xmlDocSetRootElement (doc, node);
			for(auto &group: _Groups)
			{
				group.writeTo(node);
			}
			xmlStream.flush();
			f.close();
		}
		else
		{
			nlwarning ("Can't open the file %s", userGroupFileName.c_str());

		}
	}
	catch (const NLMISC::Exception &e)
	{
		nlwarning ("Error while writing the file %s : %s.", userGroupFileName.c_str(), e.what ());
	}
}

bool CItemGroupManager::loadGroups()
{

	std::string userGroupFileName = "save/groups_" + PlayerSelectedFileName + ".xml";
	if (!NLMISC::CFile::fileExists(userGroupFileName) || NLMISC::CFile::getFileSize(userGroupFileName) == 0)
	{
		nlinfo("No item groups file found !");
		return false;
	}
	//Init loading
	NLMISC::CIFile f;
	f.open(userGroupFileName);
	NLMISC::CIXml xmlStream;
	xmlStream.init(f);
	// Actual loading
	xmlNodePtr globalEnclosing;
	globalEnclosing = xmlStream.getRootNode();
	if(!globalEnclosing)
	{
		nlwarning("no root element in item_group xml, skipping xml parsing");
		return false;
	}
	if(strcmp(( (char*)globalEnclosing->name), "item_groups"))
	{
		nlwarning("wrong root element in item_group xml, skipping xml parsing");
		return false;
	}
	xmlNodePtr curNode = globalEnclosing->children;
	while (curNode)
	{
		if (NLMISC::stricmp((char*)curNode->name, "group") == 0)
		{
			CItemGroup group;
			group.readFrom(curNode);
			_Groups.push_back(group);
		}
		curNode = curNode->next;
	}
	f.close();

	return true;
}




//move a group from all available inventory to dst
bool CItemGroupManager::moveGroup(std::string name, INVENTORIES::TInventory dst)
{
	CItemGroup* group = findGroup(name);
	if(!group)
	{
		nlinfo("group %s not found", name.c_str());
		return false;
	}
	CInventoryManager* pIM = CInventoryManager::getInstance();

	std::string moveParams = "to=lists|nblist=1|listsheet0=" + toDbPath(dst);
	// Grab all matching item from all available inventory and put it in dst
	for (int i=0; i< INVENTORIES::TInventory::NUM_ALL_INVENTORY; i ++)
	{
		INVENTORIES::TInventory inventory = (INVENTORIES::TInventory)i;
		if (inventory != dst && pIM->isInventoryAvailable(inventory))
		{

			for(auto &item : matchingItems(group, inventory))
			{
				CAHManager::getInstance()->runActionHandler("move_item", item.pCS, moveParams);
			}

		}
	}
	return true;

}


bool CItemGroupManager::equipGroup(std::string name, bool pullBefore)
{
	CItemGroup* group = findGroup(name);
	if(!group)
	{
		nlinfo("group %s not found", name.c_str());
		return false;
	}

	if(pullBefore) moveGroup(name, INVENTORIES::TInventory::bag);

	uint32 maxEquipTime = 0;

	std::map<ITEM_TYPE::TItemType, bool> possiblyDual =
	{
		{ITEM_TYPE::ANKLET, false},
		{ITEM_TYPE::BRACELET, false},
		{ITEM_TYPE::EARING, false},
		{ITEM_TYPE::RING, false},
		{ITEM_TYPE::DAGGER, false},
	};
	std::vector<CInventoryItem> duals;

	for(auto &item: matchingItems(group, INVENTORIES::TInventory::bag))
	{
		ITEM_TYPE::TItemType ItemType = item.pCS->asItemSheet()->ItemType;
		// If the item can be weared 2 times, don't automatically equip the second one
		// Or else it will simply replace the first. We'll deal with them later
		if(possiblyDual.find(ItemType) != possiblyDual.end())
		{
			if (possiblyDual[ItemType])
			{
				duals.push_back(item);
				continue;
			}
			possiblyDual[ItemType] = true;
		}
		maxEquipTime = std::max(maxEquipTime, item.pCS->asItemSheet()->EquipTime);
		CInventoryManager::getInstance()->autoEquip(item.indexInBag, true);
	}
	// Manually equip dual items
	for(auto &item : duals)
	{
		ITEM_TYPE::TItemType ItemType = item.pCS->asItemSheet()->ItemType;
		std::string dstPath = string(LOCAL_INVENTORY);
		switch(ItemType)
		{
		case ITEM_TYPE::ANKLET:
			dstPath += ":EQUIP:" + NLMISC::toString((int)SLOT_EQUIPMENT::ANKLER); break;
		case ITEM_TYPE::BRACELET:
			dstPath += ":EQUIP:" + NLMISC::toString((int)SLOT_EQUIPMENT::WRISTR);; break;
		case ITEM_TYPE::EARING:
			dstPath += ":EQUIP:" + NLMISC::toString((int)SLOT_EQUIPMENT::EARR);; break;
		case ITEM_TYPE::RING:
			dstPath += ":EQUIP:" + NLMISC::toString((int)SLOT_EQUIPMENT::FINGERR);;break;
		case ITEM_TYPE::DAGGER:
			dstPath += "HAND:1"; break;
		default:
			break;
		}
		std::string srcPath = item.pCS->getSheet();
		maxEquipTime = std::max(maxEquipTime, item.pCS->asItemSheet()->EquipTime);
		CInventoryManager::getInstance()->equip(srcPath, dstPath);
	}
	// For some reason, there is no (visual) invalidation (server still blocks any action), force one
	// Unfortunately, there is no clean way to do this, so we'll simulate one
	NLMISC::IRunnable *runnable = (NLMISC::IRunnable *)(new CFakeEquipTime((NLMISC::TGameCycle)maxEquipTime));
	NLMISC::IThread *thread = NLMISC::IThread::create(runnable);
	thread->start();
	return true;

}

bool CItemGroupManager::createGroup(std::string name)
{
	if(findGroup(name)) return false;
	CItemGroup group = CItemGroup();
	group.name = name;
	uint i;
	CDBCtrlSheet* pCS;
	for (i = 0; i < MAX_HANDINV_ENTRIES; ++i)
	{
		pCS = CInventoryManager::getInstance()->getHandSheet(i);
		if(!pCS) continue;
		NLMISC::CSheetId sheet(pCS->getSheetId());
		group.addItem(sheet.toString(), pCS->getQuality(), pCS->getItemWeight(), pCS->getItemColor());
	}


	for (i = 0; i < MAX_EQUIPINV_ENTRIES; ++i)
	{
		pCS = CInventoryManager::getInstance()->getEquipSheet(i);
		if(!pCS) continue;
		NLMISC::CSheetId sheet(pCS->getSheetId());
		group.addItem(sheet.toString(), pCS->getQuality(), pCS->getItemWeight(), pCS->getItemColor());
	}

	_Groups.push_back(group);


}
bool CItemGroupManager::deleteGroup(std::string name)
{
	std::vector<CItemGroup> tmp;
	for(auto &group: _Groups)
	{
		if(group.name == name) continue;
		tmp.push_back(group);
	}
	// Nothing removed, error
	if(tmp.size() == _Groups.size()) return false;
	_Groups = tmp;
	return true;
}

CItemGroup* CItemGroupManager::findGroup(std::string name)
{
	for(auto &group: _Groups)
	{
		if (group.name == name) return &group;
	}
	return NULL;
}
// Note : Guild & room aren't supported because missing price might cause issue
std::string CItemGroupManager::toDbPath(INVENTORIES::TInventory inventory)
{
	switch(inventory)
	{
	case INVENTORIES::TInventory::bag:
		return LIST_BAG_TEXT; break;
	case INVENTORIES::TInventory::pet_animal1:
		return LIST_PA0_TEXT; break;
	case INVENTORIES::TInventory::pet_animal2:
		return LIST_PA1_TEXT; break;
	case INVENTORIES::TInventory::pet_animal3:
		return LIST_PA2_TEXT; break;
	case INVENTORIES::TInventory::pet_animal4:
		return LIST_PA3_TEXT; break;
	default:
		return "";
	}
}

std::vector<CInventoryItem> CItemGroupManager::matchingItems(CItemGroup *group, INVENTORIES::TInventory inventory)
{
	//Not very clean, but no choice, it's ugly time
	std::vector<CInventoryItem> out;
	std::string dbPath = toDbPath(inventory);
	if(dbPath.empty())
	{
		nldebug("Inventory type %s not supported", INVENTORIES::toString(inventory).c_str());
		return out;
	}

	IListSheetBase *pList = dynamic_cast<IListSheetBase*>(CWidgetManager::getInstance()->getElementFromId(dbPath));
	for(uint i=0; i < MAX_BAGINV_ENTRIES; i++)
	{
		CDBCtrlSheet *pCS = pList->getSheet(i);
		if(group->contains(pCS))
		{

			out.push_back(CInventoryItem(pCS, inventory, i));
		}
	}

	return out;

}

// Singleton management
CItemGroupManager *CItemGroupManager::getInstance()
{
	if (!_Instance)
		_Instance = new CItemGroupManager();
	return _Instance;
}
void CItemGroupManager::releaseInstance()
{
	if (_Instance)
		delete _Instance;
	_Instance = NULL;
}
