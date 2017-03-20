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


#ifndef RY_ITEM_GROUP_MANAGER_H
#define RY_ITEM_GROUP_MANAGER_H
#include <limits>
#include "interface_v3/inventory_manager.h"
#include "interface_v3/dbctrl_sheet.h"
#include "game_share/inventories.h"
#include "game_share/slot_equipment.h"

struct CInventoryItem {
public:
	CDBCtrlSheet* pCS;
	INVENTORIES::TInventory origin;
	uint32 indexInBag;
	SLOT_EQUIPMENT::TSlotEquipment slot; // Used only for dagger (right/left hand slot)
	CInventoryItem(CDBCtrlSheet *pCS, INVENTORIES::TInventory origin, uint32 indexInBag, SLOT_EQUIPMENT::TSlotEquipment slot = SLOT_EQUIPMENT::UNDEFINED) :
		pCS(pCS), origin(origin), indexInBag(indexInBag), slot(slot) {}

};

class CItemGroup {
public:
	struct CItem {
	std::string sheetName;
	uint16 quality;
	uint32 weight;
	uint8 color;
	SLOT_EQUIPMENT::TSlotEquipment slot; // Used only for dagger (right/left hand slot)
	uint32 minPrice;
	uint32 maxPrice;
	bool usePrice;
	CItem(std::string sheetName = "", uint16 quality = 0, uint32 weight = 0, uint8 color = 0, SLOT_EQUIPMENT::TSlotEquipment slot = SLOT_EQUIPMENT::UNDEFINED, uint32 minPrice = 0, uint32 maxPrice = std::numeric_limits<uint32>::max(), bool usePrice = false) :
		sheetName(sheetName), quality(quality), weight(weight), color(color), slot(slot), minPrice(minPrice), maxPrice(maxPrice), usePrice(usePrice) {}

};

public:
	CItemGroup();

	// return true if any item in the group match the parameter ; slot is UNDEFINED unless the item has been found in the group
	bool contains(CDBCtrlSheet *other);
	bool contains(CDBCtrlSheet* other, SLOT_EQUIPMENT::TSlotEquipment &slot);
	void addItem(std::string sheetName, uint16 quality, uint32 weight, uint8 color, SLOT_EQUIPMENT::TSlotEquipment slot);
	void addRemove(std::string slotName);
	void addRemove(SLOT_EQUIPMENT::TSlotEquipment slot);
	void writeTo(xmlNodePtr node);
	void readFrom(xmlNodePtr node);

	std::string name;
	std::vector<CItem> Items;
	std::vector<SLOT_EQUIPMENT::TSlotEquipment> removeBeforeEquip;
};

class CItemGroupManager {
public:
	// Singleton management
	static CItemGroupManager* getInstance();
	static void releaseInstance();
	//Ctor
	CItemGroupManager();
	// Regular function
	void init();
	void uninit();
	void saveGroups();
	bool loadGroups();
	void linkInterface();
	void unlinkInterface();
	//Return NULL if no group was found
	//Return false if no group was found
	bool moveGroup(std::string name, INVENTORIES::TInventory dst);
	bool equipGroup(std::string name, bool pullBefore=true);
	bool createGroup(std::string name, bool removeUnequiped=false);
	bool deleteGroup(std::string name);
	void listGroup();
	std::vector<std::string> getGroupNames(CDBCtrlSheet *pCS);

private:
	CItemGroup* findGroup(std::string name);
	std::vector<CInventoryItem> matchingItems(CItemGroup* group, INVENTORIES::TInventory inventory);

	std::vector<CItemGroup> _Groups;
	std::string toDbPath(INVENTORIES::TInventory inventory);
	// Singleton's instance
	static CItemGroupManager *_Instance;
};


class CFakeEquipTime : public NLMISC::IRunnable
{
public:
	CFakeEquipTime(NLMISC::TGameCycle time) : time(time) {}
	void invalidActions();
	void validActions();
	void run();
	NLMISC::TGameCycle time;
};

#endif // RY_ITEM_GROUP_MANAGER_H
