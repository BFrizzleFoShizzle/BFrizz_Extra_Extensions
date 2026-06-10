#include <kenshi/Dialogue.h>
#include <Debug.h>
#include <core/Functions.h>
#include <kenshi/Character.h>
#include <kenshi/GameData.h>
#include <kenshi/Gear.h>
#include <kenshi/CharStats.h>
#include <kenshi/Inventory.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Platoon.h>
#include <kenshi/WorldEventStateQuery.h>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <kenshi/Faction.h>
#include <kenshi/gui/ManagementScreen.h>

enum itemTypeEx
{
	VARIABLE = 1000
};

enum TalkerEnumEx
{
	T_NOTIFICATION = 1000
};

enum DialogConditionEnumEx
{
	DC_IS_SLEEPING = 1000,
	DC_HAS_SHORT_TERM_TAG,
	DC_IS_ALLY_BECAUSE_OF_DISGUISE,
	DC_STAT_LEVEL_UNMODIFIED,
	DC_STAT_LEVEL_MODIFIED,
	DC_WEAPON_LEVEL,
	DC_ARMOUR_LEVEL
};

// WorldEventStateQuery objects don't store a ref to their gamedata so we need this to get it in WorldEventStateQuery::isTrue
// I'm currently not doing garbage collection on this but that should just cause a small memory leak when reloading, not enough to care about
boost::unordered_map<WorldEventStateQuery*, GameData*> queryMap;
// probably unnecessary  but I don't want to take the risk
boost::mutex mapLock;

WorldEventStateQuery* (*getFromData_orig)(GameData* d);
WorldEventStateQuery* getFromData_hook(GameData* d)
{
	WorldEventStateQuery* query = getFromData_orig(d);

	// lock + add to map
	boost::lock_guard<boost::mutex> lock(mapLock);
	queryMap.emplace(query, d);

	return query;
}

static const float SQUAD_CHECK_RADIUS = 900.0f;

// TODO remove?
static bool DialogCompare(int val1, int val2, ComparisonEnum compareBy)
{

	if (compareBy == ComparisonEnum::CE_EQUALS && val1 == val2)
		return true;
	if (compareBy == ComparisonEnum::CE_LESS_THAN && val1 < val2)
		return true;
	if (compareBy == ComparisonEnum::CE_MORE_THAN && val1 > val2)
		return true;

	return false;
}

static bool DialogCompare(int val1, DialogLineData::DialogCondition* condition)
{

	if (condition->compareBy == ComparisonEnum::CE_EQUALS && val1 == condition->value)
		return true;
	if (condition->compareBy == ComparisonEnum::CE_LESS_THAN && val1 < condition->value)
		return true;
	if (condition->compareBy == ComparisonEnum::CE_MORE_THAN && val1 > condition->value)
		return true;

	return false;
}

static bool checkCondition(Character* characterCheck, Character* characterTarget, DialogLineData::DialogCondition* condition)
{
	switch (condition->key)
	{
		case DC_IS_SLEEPING:
			if (!DialogCompare(characterCheck->inSomething == UseStuffState::IN_BED, condition))
				return false;
			break;
		case DC_IS_ALLY_BECAUSE_OF_DISGUISE:
			if (!DialogCompare((characterCheck->isAlly(characterTarget, true) && !characterCheck->isAlly(characterTarget, false)), condition))
				return false;
			break;
		case DC_WEAPON_LEVEL:
		{
			// Note: value is -1 if unarmed
			// this check often doesn't check equipped weapons on back
			Weapon* weapon = characterCheck->getCurrentWeapon();
			// this seems to be the same
			if (!weapon)
				weapon = characterCheck->getThePreferredWeapon();
			int level = weapon == nullptr ? -1 : weapon->getLevel();
			if (!weapon)
			{
				lektor<InventorySection*> sections;
				characterCheck->inventory->getAllSectionsOfType(sections, AttachSlot::ATTACH_WEAPON);
				for (int i = 0; i < sections.size(); ++i)
				{
					const Ogre::vector<InventorySection::SectionItem>::type& items = sections[i]->getItems();
					for (int j = 0; j < items.size(); ++j)
						if (weapon = dynamic_cast<Weapon*>(items[j].item))
							level = std::max(level, weapon->getLevel());
				}

				// cleanup
				free(sections.stuff);
			}

			if (!DialogCompare(level, condition))
				return false;
			break;
		}
		case DC_ARMOUR_LEVEL:
		{
			// Note: value is -1 if unarmoured
			lektor<Item*> armour;
			armour.maxSize = 0;
			armour.count = 0;
			armour.stuff = nullptr;
			characterCheck->getInventory()->getEquippedArmour(armour);
			bool hasMatch = false;
			// check if any equipped armour meets condition
			for (int i = 0; i < armour.size(); ++i)
				if (DialogCompare(armour[i]->getLevel(), condition))
					hasMatch = true;
			// unarmoured
			if (armour.size() == 0)
				hasMatch = DialogCompare(-1, condition);
			// garbage collect
			if (armour.stuff)
				free(armour.stuff);
			// return false if no armour matches
			if (!hasMatch)
				return false;
			break;
		}
	}
	return true;
}

std::vector<std::pair<Inventory*, lektor<Item*>>> GetItemsOfTypeFromSections(Character* character, itemType type)
{
	std::vector<std::pair<Inventory*, lektor<Item*>>> sectionItems;
	sectionItems.emplace_back(std::make_pair(character->inventory, lektor<Item*>()));
	for (ogre_unordered_map<std::string, InventorySection*>::type::iterator iter = character->inventory->sections.begin();
		iter != character->inventory->sections.end(); ++iter)
	{
		iter->second->getAllItemsOfType(sectionItems[0].second, type);
		// also search backpacks in any inventory section
		lektor<Item*> containers;
		iter->second->getAllItemsOfType(containers, itemType::CONTAINER);
		for (int i = 0; i < containers.size(); ++i)
		{
			sectionItems.emplace_back(std::make_pair(containers[i]->getInventory(), lektor<Item*>()));
			containers[i]->getInventory()->getAllItemsOfType(sectionItems.back().second, type, false);
		}
		free(containers.stuff);
	}

	return sectionItems;
}

const GameDataReference* FindInList(GameData* target, const Ogre::vector<GameDataReference>::type* list)
{
	for (int i = 0; i < list->size(); ++i)
	{
		if ((*list)[i].ptr == target)
			return &(*list)[i];
	}
	return nullptr;
}

// searches all sections + backpacks
Item* FindItemInCharacterInventories(Character* character, GameData* itemData, const Ogre::vector<GameDataReference>::type* armourFactions
	, const Ogre::vector<GameDataReference>::type* swordManufacturer, const Ogre::vector<GameDataReference>::type* swordModel, Inventory*& inventory)
{
	std::vector<std::pair<Inventory*, lektor<Item*>>> sectionItems = GetItemsOfTypeFromSections(character, itemData->type);

	Item* item = nullptr;
	inventory = nullptr;
	// find matching item
	for (int j = 0; j < sectionItems.size(); ++j)
	{
		if (!item)
		{
			inventory = sectionItems[j].first;
			lektor<Item*>& items = sectionItems[j].second;
			for (int i = 0; i < items.size(); ++i)
			{
				if (items[i]->getGameData() == itemData)
				{
					if (itemData->type == itemType::WEAPON)
					{
						// Extra weapon checks
						if ((swordManufacturer == nullptr || FindInList(items[i]->manufacturerData, swordManufacturer))
							&& (swordModel == nullptr || FindInList(items[i]->materialData, swordModel)))
						{
							item = items[i];
							break;
						}
					}
					else if (itemData->type == itemType::ARMOUR)
					{
						// Extra armour checks
						if (armourFactions == nullptr || (items[i]->isAFactionUniform() != nullptr && FindInList(items[i]->isAFactionUniform()->data, armourFactions)))
						{
							item = items[i];
							break;
						}
					}
					else
					{
						// default case
						item = items[i];
						break;
					}
				}
			}
		}
		// manual free
		free(sectionItems[j].second.stuff);
	}

	return item;
}

DialogLineData* (*DialogLineData_CONSTRUCTOR_orig)(DialogLineData* thisptr, GameData* dat);
DialogLineData* DialogLineData_CONSTRUCTOR_hook(DialogLineData* thisptr, GameData* dat)
{
	DialogLineData_CONSTRUCTOR_orig(thisptr, dat);
	// clear hasItem, we implement this ourselves
	thisptr->hasItem.count = 0;
	return thisptr;
}

// 0 = no target level
int GetItemCount(Character* characterCheck, GameData* targetItem, itemType itemType, int targetLevel, const Ogre::vector<GameDataReference>::type* armourFactions
	, const Ogre::vector<GameDataReference>::type* swordManufacturer, const Ogre::vector<GameDataReference>::type* swordModel)
{
	int count = 0;
	std::vector<std::pair<Inventory*, lektor<Item*>>> sectionItems = GetItemsOfTypeFromSections(characterCheck, itemType);
	for (int j = 0; j < sectionItems.size(); ++j)
	{
		for (int k = 0; k < sectionItems[j].second.size(); ++k)
		{
			Item* inventoryItem = sectionItems[j].second[k];
			// skip incorrect target level
			if (targetLevel != 0 && std::max(0, targetLevel) != inventoryItem->getLevel())
				continue;

			if (targetItem == nullptr || inventoryItem->data == targetItem)
			{
				if (itemType == itemType::WEAPON)
				{
					// Extra weapon checks
					if ((swordManufacturer == nullptr || FindInList(inventoryItem->manufacturerData, swordManufacturer))
						&& (swordModel == nullptr || FindInList(inventoryItem->materialData, swordModel)))
					{
						count += inventoryItem->quantity;
					}
				}
				else if (itemType == itemType::ARMOUR)
				{
					// Extra armour checks
					if (armourFactions == nullptr || (inventoryItem->isAFactionUniform() != nullptr && FindInList(inventoryItem->isAFactionUniform()->data, armourFactions)))
					{
						count += inventoryItem->quantity;
					}
				}
				else
				{
					count += inventoryItem->quantity;
				}
			}
		}
	}
	return count;
}

bool (*DialogLineData_checkConditions_orig)(DialogLineData* thisptr, Dialogue* dialog, Character* target, bool isWordswap);
bool DialogLineData_checkConditions_hook(DialogLineData* thisptr, Dialogue* dialog, Character* target, bool isWordswap)
{
	// T_ME behaviour - do I have memory tag for target
	Character* characterCheck = dialog->getCharacter();
	// I'm sometimes getting NO TARGET?
	Character* characterTarget = target;

	if (thisptr->speaker != TalkerEnum::T_ME && thisptr->speaker != TalkerEnum::T_WHOLE_SQUAD)
	{
		// swap
		Character* temp = characterTarget;
		characterTarget = characterCheck;
		characterCheck = temp;
	}

	for (DialogLineData::DialogCondition** condition = thisptr->conditions.begin(); condition < thisptr->conditions.end(); ++condition)
	{
		if (!characterCheck)
		{
			ErrorLog("NO SPEAKER");
			break;
		}

		if (!characterTarget)
		{
			ErrorLog("NO TARGET");
			// TODO is this a problem?
			//break;
		}

		if (thisptr->speaker == TalkerEnum::T_WHOLE_SQUAD)
		{
			// with above branch, characterCheck will be "me"
			ActivePlatoon* activePlatoon = characterCheck->getPlatoon();
			lektor<RootObject*> characters;
			// couldn't find T_WHOLE_SQUAD radius but interjection radius is similar and appears to be 900
			activePlatoon->getCharactersInArea(characters, characterCheck->getPosition(), SQUAD_CHECK_RADIUS, false);

			// if any
			bool found = false;
			for (int i = 0; i < characters.size(); ++i)
			{
				Character* squadChar = dynamic_cast<Character*>(characters[i]);
				if (squadChar)
				{
					if (checkCondition(squadChar, characterTarget, *condition))
						// condition is met -  move on to the next condition
						found = true;
					//break;
				}
			}

			// cleanup
			if (characters.stuff)
				free(characters.stuff);

			if (!found)
				return false;
		}
		else
		{
			if (!checkCondition(characterCheck, characterTarget, *condition))
				return false;
		}
	}

	// find restrictions
	const Ogre::vector<GameDataReference>::type* armourFactions = thisptr->getGameData()->getReferenceListIfExists("armour faction");
	const Ogre::vector<GameDataReference>::type* swordManufacturer = thisptr->getGameData()->getReferenceListIfExists("sword manufacturer");
	const Ogre::vector<GameDataReference>::type* swordModel = thisptr->getGameData()->getReferenceListIfExists("sword model");
	const Ogre::vector<GameDataReference>::type* targetHasItemList = thisptr->getGameData()->getReferenceListIfExists("target has item");
	
	// check weapon/armour item conditions
	if (targetHasItemList)
	{
		for (int i = 0; i < targetHasItemList->size(); ++i)
		{
			const GameDataReference& targetItem = targetHasItemList->at(i);
			// 0 = no target level
			int targetLevel = targetItem.values[1];
			int count = GetItemCount(characterCheck, targetItem.ptr, targetItem.ptr->type, targetLevel, armourFactions, swordManufacturer, swordModel);
			/*
			int count = 0;
			std::vector<std::pair<Inventory*, lektor<Item*>>> sectionItems = GetItemsOfTypeFromSections(characterCheck, targetItem.ptr->type);
			for (int j = 0; j < sectionItems.size(); ++j)
			{
				for (int k = 0; k < sectionItems[j].second.size(); ++k)
				{
					Item* inventoryItem = sectionItems[j].second[k];
					// skip incorrect target level
					if (targetLevel != 0 && std::max(0, targetLevel) != inventoryItem->getLevel())
						continue;

					if (inventoryItem->data == targetItem.ptr)
					{
						DebugLog("Match");
						if (targetItem.ptr->type == itemType::WEAPON)
						{
							// Extra weapon checks
							if ((swordManufacturer == nullptr || FindInList(inventoryItem->manufacturerData, swordManufacturer))
								&& (swordModel == nullptr || FindInList(inventoryItem->materialData, swordModel)))
							{
								DebugLog("Weapon check passed");
								count += inventoryItem->quantity;
							}
						}
						else if (targetItem.ptr->type == itemType::ARMOUR)
						{
							// Extra armour checks
							if (armourFactions == nullptr || (inventoryItem->isAFactionUniform() != nullptr && FindInList(inventoryItem->isAFactionUniform()->data, armourFactions)))
							{
								count += inventoryItem->quantity;
							}
						}
						else
						{
							count += inventoryItem->quantity;
						}
					}
				}
			}
			*/
			// if there aren't enough matching items, fail the check
			if (count < targetItem.values[0])
				return false;
		}
	}

	const Ogre::vector<GameDataReference>::type* targetHasItemTypeList = thisptr->getGameData()->getReferenceListIfExists("target has item type");
	if (targetHasItemTypeList)
	{
		for (int i = 0; i < targetHasItemTypeList->size(); ++i)
		{
			const GameDataReference& targetItem = targetHasItemTypeList->at(i);
			// 0 = no target level
			int targetLevel = targetItem.values[1];
			int count = GetItemCount(characterCheck, nullptr, targetItem.ptr->type, targetLevel, armourFactions, swordManufacturer, swordModel);
			if (count < targetItem.values[0])
				return false;
		}
	}

	return DialogLineData_checkConditions_orig(thisptr, dialog, target, isWordswap);
}

bool checkTag(DialogConditionEnumEx dialogCondition, Character* conditionCheck, Character* conditionTarget, ComparisonEnum compareBy, int tag, int value)
{
	if (dialogCondition == DialogConditionEnumEx::DC_HAS_SHORT_TERM_TAG)
	{
		return DialogCompare(conditionCheck->getCharacterMemoryTag(conditionTarget, (CharacterPerceptionTags_ShortTerm)tag), value, compareBy);
	}
	// both of these can be done together
	else if (dialogCondition == DialogConditionEnumEx::DC_STAT_LEVEL_MODIFIED
		|| dialogCondition == DialogConditionEnumEx::DC_STAT_LEVEL_UNMODIFIED)
	{
		// swap between enum behaviour
		bool unmodified = dialogCondition == DialogConditionEnumEx::DC_STAT_LEVEL_UNMODIFIED;

		float stat = conditionCheck->getStats()->getStat((StatsEnumerated)tag, unmodified);
		return DialogCompare((int)stat, value, compareBy);
	}

	// not our problem
	return true;
}

bool (*checkTags_orig)(DialogLineData* thisptr, Character* me, Character* target);
bool checkTags_hook(DialogLineData* thisptr, Character* me, Character* target)
{
	// TAG CONDITIONS
	for (int i = 0; i < thisptr->conditions.size(); ++i)
	{
		// optimization - only do a full check if there's an actual extended condition
		if (thisptr->conditions[i]->key == DialogConditionEnumEx::DC_HAS_SHORT_TERM_TAG
			|| thisptr->conditions[i]->key == DC_STAT_LEVEL_UNMODIFIED
			|| thisptr->conditions[i]->key == DC_STAT_LEVEL_MODIFIED)
		{
			const Ogre::vector<GameDataReference>::type* list = thisptr->data->getReferenceListIfExists("conditions");
			if (list)
			{
				for (int i = 0; i < list->size(); ++i)
				{
					GameData* gameData = (*list)[i].ptr;
					DialogConditionEnumEx dialogCondition = (DialogConditionEnumEx)gameData->idata.find("condition name")->second;
					ComparisonEnum compareBy = (ComparisonEnum)gameData->idata.find("compare by")->second;
					TalkerEnum who = (TalkerEnum)gameData->idata.find("who")->second;
					int tag = gameData->idata.find("tag")->second;
					int value = (*list)[i].values[0];

					// T_ME behaviour - do I have tag for target
					Character* conditionCheck = me;
					// Note: target can sometimes be null, seems to happen on interjection nodes
					Character* conditionTarget = target;
					if (who != TalkerEnum::T_ME && who != TalkerEnum::T_WHOLE_SQUAD)
					{
						// swap
						Character* temp = conditionTarget;
						conditionTarget = conditionCheck;
						conditionCheck = temp;
					}

					if (who == TalkerEnum::T_WHOLE_SQUAD)
					{
						ActivePlatoon* platoon = conditionCheck->getPlatoon();
						if (platoon)
						{
							lektor<RootObject*> characters;
							// couldn't find T_WHOLE_SQUAD radius but interjection radius is similar and appears to be 900
							platoon->getCharactersInArea(characters, conditionCheck->getPosition(), SQUAD_CHECK_RADIUS, false);

							bool found = false;
							for (int i = 0; i < characters.size(); ++i)
							{
								Character* squadChar = dynamic_cast<Character*>(characters[i]);
								// if any
								if (squadChar)
								{
									if (checkTag(dialogCondition, squadChar, conditionTarget, compareBy, tag, value))
										// condition is met -  move on to the next condition
										found = true;
									//break;
								}
							}

							// cleanup
							if (characters.stuff)
								free(characters.stuff);

							if (!found)
								return false;
						}
					}
					else
					{
						if (!checkTag(dialogCondition, conditionCheck, conditionTarget, compareBy, tag, value))
							return false;
					}
				}
			}
			// above loop should check all conditions
			break;
		}
	}

	// VARIABLES
	ogre_unordered_map<std::string, Ogre::vector<GameDataReference>::type>::type::iterator iter = thisptr->getGameData()->objectReferences.find("variable equals");
	if (iter != thisptr->getGameData()->objectReferences.end())
	{
		for (Ogre::vector<GameDataReference>::type::iterator variableIter = iter->second.begin(); variableIter != iter->second.end(); ++variableIter)
		{
			ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");
			if (valueIter != variableIter->ptr->idata.end() && !(valueIter->second == variableIter->values[0]))
				return false;
		}
	}

	iter = thisptr->getGameData()->objectReferences.find("variable less than");
	if (iter != thisptr->getGameData()->objectReferences.end())
	{
		for (Ogre::vector<GameDataReference>::type::iterator variableIter = iter->second.begin(); variableIter != iter->second.end(); ++variableIter)
		{
			ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");
			if (valueIter != variableIter->ptr->idata.end() && !(valueIter->second < variableIter->values[0]))
				return false;
		}
	}

	iter = thisptr->getGameData()->objectReferences.find("variable greater than");
	if (iter != thisptr->getGameData()->objectReferences.end())
	{
		for (Ogre::vector<GameDataReference>::type::iterator variableIter = iter->second.begin(); variableIter != iter->second.end(); ++variableIter)
		{
			ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");
			if (valueIter != variableIter->ptr->idata.end() && !(valueIter->second > variableIter->values[0]))
				return false;
		}
	}

	// VANILLA TAGS
	return checkTags_orig(thisptr, me, target);
}

// returns num items taken
int takeItems(Character* giver, Character* taker, GameData* itemData, int count, const Ogre::vector<GameDataReference>::type* armourFactions
	, const Ogre::vector<GameDataReference>::type* swordManufacturer, const Ogre::vector<GameDataReference>::type* swordModel)
{
	int countTaken = 0;

	while (true)
	{
		Inventory* inventory;
		Item* item = FindItemInCharacterInventories(giver, itemData, armourFactions, swordManufacturer, swordModel, inventory);

		// stop when the character no longer has instances of the item
		if (item == nullptr)
			return countTaken;

		// TODO can we use inventory->removeItemDontDestroy_returnsItem ? 

		const int countRemaining = count - countTaken;
		// if stack has more items than we're removing, split and return
		if (item->quantity > countRemaining)
		{
			// part of stack is moved, split by creating new instance
			item->quantity -= countRemaining;
			Item* newItem = ou->theFactory->copyItem(item);
			newItem->quantity = countRemaining;
			taker->giveItem(newItem, true, false);
			return count;
		}

		// whole stack is moved
		item = inventory->removeItemDontDestroy_returnsItem(item, item->quantity, true);
		if (item == nullptr)
		{
			ErrorLog("Error taking item, removeItemDontDestroy_returnsItem returned false");
			return countTaken;
		}
		countTaken += item->quantity;
		
		taker->giveItem(item, true, false);
	}

	// count is reached
	return count;
}

// returns num items destroyed
int destroyItems(Character* target, GameData* itemData, int count, const Ogre::vector<GameDataReference>::type* armourFactions
	, const Ogre::vector<GameDataReference>::type* swordManufacturer, const Ogre::vector<GameDataReference>::type* swordModel)
{
	while (true)
	{

		Inventory* inventory;
		Item* item = FindItemInCharacterInventories(target, itemData, armourFactions, swordManufacturer, swordModel, inventory);

		// stop when the character no longer has instances of the item
		if (item == nullptr)
			return count;

		if (item->quantity > count)
		{
			item->quantity -= count;
			return count;
		}

		count -= item->quantity;

		if (!inventory->removeItemAutoDestroy(item, item->quantity))
			DebugLog("removeItem returned false");
	}
	return count;
}

void doRefAction(const std::string& action, DialogLineData* line, Dialogue* thisptr)
{
	const Ogre::vector<GameDataReference>::type* ref = line->getGameData()->getReferenceListIfExists(action);
	if (ref == nullptr)
		return;

	if (ref->size() == 0)
	{
		ErrorLog("Missing references for \"" + action + "\"");
	}

	if (action == "take item" || action == "take item from squad")
	{
		// find restrictions
		const Ogre::vector<GameDataReference>::type* armourFactions = line->getGameData()->getReferenceListIfExists("armour faction");
		const Ogre::vector<GameDataReference>::type* swordManufacturer = line->getGameData()->getReferenceListIfExists("sword manufacturer");
		const Ogre::vector<GameDataReference>::type* swordModel = line->getGameData()->getReferenceListIfExists("sword model");

		for (Ogre::vector<GameDataReference>::type::const_iterator itemIter = ref->begin(); itemIter != ref->end(); ++itemIter)
		{
			Character* giver = thisptr->getConversationTarget().getCharacter();
			Character* taker = thisptr->me;
			if (giver != nullptr && taker != nullptr)
			{
				if (action == "take item")
				{
					takeItems(giver, taker, itemIter->ptr, itemIter->values[0], armourFactions, swordManufacturer, swordModel);
				}
				else
				{
					ActivePlatoon* activePlatoon = giver->getPlatoon();
					lektor<RootObject*> characters;
					// couldn't find T_WHOLE_SQUAD radius but interjection radius is similar and appears to be 900
					activePlatoon->getCharactersInArea(characters, taker->getPosition(), SQUAD_CHECK_RADIUS, false);

					int itemsLeft = itemIter->values[0];
					for (int c = 0; c < characters.size(); ++c)
					{
						Character* squadChar = dynamic_cast<Character*>(characters[c]);
						if (squadChar)
							itemsLeft -= takeItems(giver, taker, itemIter->ptr, itemsLeft, armourFactions, swordManufacturer, swordModel);
						if (itemsLeft == 0)
							break;
					}

					// cleanup
					free(characters.stuff);
				}
			}
		}
	}
	else if (action == "destroy item" || action == "destroy item from squad")
	{
		// find restrictions
		const Ogre::vector<GameDataReference>::type* armourFactions = line->getGameData()->getReferenceListIfExists("armour faction");
		const Ogre::vector<GameDataReference>::type* swordManufacturer = line->getGameData()->getReferenceListIfExists("sword manufacturer");
		const Ogre::vector<GameDataReference>::type* swordModel = line->getGameData()->getReferenceListIfExists("sword model");

		for (Ogre::vector<GameDataReference>::type::const_iterator itemIter = ref->begin(); itemIter != ref->end(); ++itemIter)
		{
			Character* target = thisptr->getConversationTarget().getCharacter();
			if (target != nullptr)
			{
				if (action == "destroy item")
				{
					destroyItems(target, itemIter->ptr, itemIter->values[0], armourFactions, swordManufacturer, swordModel);
				}
				else
				{
					ActivePlatoon* activePlatoon = target->getPlatoon();
					lektor<RootObject*> characters;
					// couldn't find T_WHOLE_SQUAD radius but interjection radius is similar and appears to be 900
					activePlatoon->getCharactersInArea(characters, target->getPosition(), SQUAD_CHECK_RADIUS, false);

					int itemsLeft = itemIter->values[0];
					for (int c = 0; c < characters.size(); ++c)
					{
						Character* squadChar = dynamic_cast<Character*>(characters[c]);
						if (squadChar)
							itemsLeft -= destroyItems(target, itemIter->ptr, itemsLeft, armourFactions, swordManufacturer, swordModel);
						if (itemsLeft == 0)
							break;
					}

					// cleanup
					free(characters.stuff);
				}
			}
		}
	}
}

bool checkWorldStateVariables(const std::string &condition, Ogre::vector<GameDataReference>::type& ref)
{
	for (Ogre::vector<GameDataReference>::type::iterator variableIter = ref.begin(); variableIter != ref.end(); ++variableIter)
	{
		int targetVal = variableIter->values[0];
		ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");

		if (valueIter != variableIter->ptr->idata.end())
		{
			// if condition isn't met, return false
			if(condition == "variable equals" && !(valueIter->second == targetVal))
				return false;
			if (condition == "variable less than" && !(valueIter->second < targetVal))
				return false;
			if (condition == "variable greater than" && !(valueIter->second > targetVal))
				return false;
		}
		else
		{
			ErrorLog("WorldStates: Variable is missing value");
		}
	}

	// all checks passed
	return true;
}

bool (*WorldEventStateQuery_isTrue_orig)(WorldEventStateQuery* thisptr);
bool WorldEventStateQuery_isTrue_hook(WorldEventStateQuery* thisptr)
{
	// regular conditions
	bool state = WorldEventStateQuery_isTrue_orig(thisptr);

	GameData* gameData;
	// prevent race conditions
	{
		boost::lock_guard<boost::mutex> lock(mapLock);
		gameData = queryMap[thisptr];
	}

	// our new conditions
	ogre_unordered_map<std::string, Ogre::vector<GameDataReference>::type>::type::iterator iter = gameData->objectReferences.find("variable equals");
	if (iter != gameData->objectReferences.end() && !checkWorldStateVariables("variable equals", iter->second))
		return false;

	iter = gameData->objectReferences.find("variable less than");
	if (iter != gameData->objectReferences.end() && !checkWorldStateVariables("variable less than", iter->second))
		return false;

	iter = gameData->objectReferences.find("variable greater than");
	if (iter != gameData->objectReferences.end() && !checkWorldStateVariables("variable greater than", iter->second))
		return false;

	return state;
}

void changeWorldStateVariable(const std::string& action, DialogLineData* line)
{
	const Ogre::vector<GameDataReference>::type* ref = line->getGameData()->getReferenceListIfExists(action);
	if (ref == nullptr)
		return;

	if (ref->size() == 0)
	{
		ErrorLog("Missing references for \"" + action + "\"");
	}

	for (Ogre::vector<GameDataReference>::type::const_iterator variableIter = ref->begin(); variableIter != ref->end(); ++variableIter)
	{
		ogre_unordered_map<std::string, int>::type::iterator valueIter = variableIter->ptr->idata.find("value");

		if (valueIter != variableIter->ptr->idata.end())
		{
			if(action == "set variable")
				valueIter->second = variableIter->values[0];
			else if (action == "add to variable")
				valueIter->second += variableIter->values[0];
		}
		else
		{
			ErrorLog("Missing parameter: value");
		}
	}
}

// info needed for constructing a weapon
class PackagedItem
{
public:
	virtual ~PackagedItem() {};
	GameData* item;
	GameData* dialogLine;
	int levelOverride;
	PackagedItem()
		: item(nullptr)
		, dialogLine(nullptr)
		, levelOverride(0)
	{

	};
	GameData* getItem()
	{
		// order of weapon args is weird
		// createItem(manufacturer,handle,item,model,level,flag)
		if (item->type == itemType::WEAPON)
		{
			if (dialogLine->getReferenceListIfExists("sword manufacturer"))
				return ou->theFactory->chooseDataFromListWithVals(dialogLine, "sword manufacturer", itemType::WEAPON_MANUFACTURER, 0)->ptr;
			// default to unknown manufacturer
			return ou->gamedata.getDataByName("Unknown", itemType::WEAPON_MANUFACTURER);
		}
		else
		{
			return item;
		}
	}
	GameData* getMesh()
	{
		if (item->type == itemType::WEAPON)
			return item;
		return nullptr;
	}
	GameData* getMatData()
	{
		if (item->type == itemType::WEAPON && dialogLine->getReferenceListIfExists("sword model"))
			return ou->theFactory->chooseDataFromListWithVals(dialogLine, "sword model", itemType::MATERIAL_SPECS_WEAPON, 0)->ptr;
		return nullptr;
	}
	Faction* getFaction()
	{
		if (item->type == itemType::ARMOUR && dialogLine->getReferenceListIfExists("armour faction"))
			return ou->factionMgr->getOrCreateFaction(ou->theFactory->chooseDataFromListWithVals(dialogLine, "armour faction", itemType::FACTION, 0)->ptr);
		return nullptr;
	}
};

void (*_doActions_orig)(Dialogue* thisptr, DialogLineData* dialogLine);
void _doActions_hook(Dialogue* thisptr, DialogLineData* dialogLine)
{
	// Notifications
	if (dialogLine->speaker == T_NOTIFICATION && dialogLine->lineCount >= 1)
	{
		std::string notification = dialogLine->texts[0];
		for (int i = 1; i < dialogLine->lineCount; ++i)
			notification += "\n" + dialogLine->texts[i];
		ou->showPlayerAMessage_withLog(notification.c_str(), true);
	}

	// DIALOGUE EFFECTS
	doRefAction("take item", dialogLine, thisptr);
	doRefAction("take item from squad", dialogLine, thisptr);
	doRefAction("destroy item", dialogLine, thisptr);
	doRefAction("destroy item from squad", dialogLine, thisptr);

	// VARIABLE EFFECTS
	changeWorldStateVariable("set variable", dialogLine);
	changeWorldStateVariable("add to variable", dialogLine);

	// prepare item creation
	for (int i = 0; i < dialogLine->givesItem.size(); ++i)
	{
		if (strcmp(typeid(*dialogLine->givesItem[i].data).name(), "class GameData") == 0)
		{
			// generic item stuff
			//DebugLog("item: " + dialogLine->givesItem[i].data->name);
			PackagedItem* item = new PackagedItem();
			item->item = dialogLine->givesItem[i].data;
			item->dialogLine = dialogLine->getGameData();
			const Ogre::vector<GameDataReference>::type* giveItemList = dialogLine->getGameData()->getReferenceListIfExists("give item");
			if (giveItemList && giveItemList->size() >= i)
				item->levelOverride = giveItemList->at(i).values[1];
			
			dialogLine->givesItem[i].data = (GameData*)item;
		}
	}

	// continue
	_doActions_orig(thisptr, dialogLine);
}

Item* (*RootObjectFactory_createItem_orig)(RootObjectFactory* thisptr, GameData* gd, const hand& handle, GameData* weaponMesh, GameData* matData, int levelOverride, Faction* flagUniform);
Item* RootObjectFactory_createItem_hook(RootObjectFactory* thisptr, GameData* gd, const hand& handle, GameData* weaponMesh, GameData* matData, int levelOverride, Faction* flagUniform)
{
	// Workaround for types not being detected properly in "undefined behaviour" typecasts
	// dynamic_cast doesn't detect class properly, but RTTI still works, so we implement our own equivalent
	if (strcmp(typeid(*gd).name(), "class PackagedItem") == 0)
	{
		PackagedItem* item = (PackagedItem*)gd;
		gd = item->getItem();
		if (item->levelOverride != 0)
			levelOverride = std::max(0, item->levelOverride);
		if (item->item->type == itemType::WEAPON)
		{
			weaponMesh = item->getMesh();
			matData = item->getMatData();
		}
		if (item->item->type == itemType::ARMOUR)
			flagUniform = item->getFaction();
	}
	return RootObjectFactory_createItem_orig(thisptr, gd, handle, weaponMesh, matData, levelOverride, flagUniform);
}

// this is a convenient place to hook into the save system
// not 100% sure this is the best way to save data - data here is written to "quick.save"
void (*saveGameState_orig)(FactionManager* thisptr, GameDataContainer* container);
void saveGameState_hook(FactionManager* thisptr, GameDataContainer* container)
{
	// find VARIABLE gamedata
	ogre_unordered_map<std::string, GameData*>::type::iterator iter = ou->gamedata.gamedataSID.begin();
	for (; iter != ou->gamedata.gamedataSID.end(); ++iter)
	{
		if (iter->second->type == (itemType)VARIABLE)
		{
			// create new GameData in save
			GameData* newGameData2 = container->createNewData(iter->second->type, iter->second->stringID, iter->second->name);
			if (newGameData2)
				newGameData2->updateFrom(iter->second, false);
		}
	}
	saveGameState_orig(thisptr, container);
}

// save data is loaded into ou->savedata but the dialogue/world state reference is to the object in ou->gamedata
// so we copy the value over on load
// this probably isn't the ideal function to hook but as far as I can tell it's only called when loading a save game
void (*loadAllPlatoons_orig)(GameWorld* thisptr);
void loadAllPlatoons_hook(GameWorld* thisptr)
{
	loadAllPlatoons_orig(thisptr);

	ogre_unordered_map<std::string, GameData*>::type::iterator iter = ou->savedata.gamedataSID.begin();
	for (; iter != ou->savedata.gamedataSID.end(); ++iter)
	{
		if (iter->second->type == (itemType)VARIABLE)
		{
			ou->gamedata.updateData(iter->second, false);
		}
	}
}

Character* (*Dialogue_getSpeaker_orig)(Dialogue* thisptr, TalkerEnum who, DialogLineData* line, bool isForWordswaps);
Character* Dialogue_getSpeaker_hook(Dialogue* thisptr, TalkerEnum who, DialogLineData* line, bool isForWordswaps)
{
	if (who == T_NOTIFICATION)
		who = T_ME;
	return Dialogue_getSpeaker_orig(thisptr, who, line, isForWordswaps);;
}

void (*DialogLineData_getText_orig)(DialogLineData* thisptr, std::string& out, bool _stampTime);
void DialogLineData_getText_hook(DialogLineData* thisptr, std::string& out, bool _stampTime)
{
	boost::unordered_map<const std::string, std::string>::iterator text0 = thisptr->getGameData()->sdata.find("text0");
	// if silent or notification - first line is empty = silent line
	if ((text0 != thisptr->getGameData()->sdata.end() && text0->second == "")
		|| thisptr->speaker == T_NOTIFICATION)
		out = "[...]";
	else
		DialogLineData_getText_orig(thisptr, out, _stampTime);
}

bool debug = true;

bool (*Dialogue_sayLine_orig)(Dialogue* thisptr, DialogLineData* line);
bool Dialogue_sayLine_hook(Dialogue* thisptr, DialogLineData* line)
{
	if (line)
	{
		boost::unordered_map<const std::string, std::string>::iterator text0 = line->getGameData()->sdata.find("text0");
		// first line is empty = silent line
		bool isSilent = text0 != line->getGameData()->sdata.end() && text0->second == "";
		
		if (isSilent || line->speaker == T_NOTIFICATION)
		{
			if (isSilent && debug)
			{
				std::string text;
				DialogLineData_getText_orig(line, text, false);
				ManagementScreen::getSingleton()->addMessage("(silent) " + thisptr->getSpeaker(line->speaker, line, false)->getName(), text, MessageLogColor::ML_SYSTEM);
			}
			thisptr->currentLine = line;
			thisptr->_doActions(line);
			DialogLineData* next = thisptr->_chooseDialog(line->children, thisptr->conversationTarget.getCharacter(), false);
			return thisptr->sayLine(next);
		}
	}
	return Dialogue_sayLine_orig(thisptr, line);
}

__declspec(dllexport) void startPlugin()
{
	// there's no obvious way to track which GameData is associated with which WorldEventStateQuery/List so we make our own
	// NOTE: there's no garage collection...
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&WorldEventStateQuery::getFromData), &getFromData_hook, &getFromData_orig))
		DebugLog("WorldStates: Could not hook function!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&WorldEventStateQuery::isTrue), &WorldEventStateQuery_isTrue_hook, &WorldEventStateQuery_isTrue_orig))
		DebugLog("WorldStates: Could not hook function!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DialogLineData::checkConditions), &DialogLineData_checkConditions_hook, &DialogLineData_checkConditions_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DialogLineData::checkTags), &checkTags_hook, &checkTags_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Dialogue::_doActions), &_doActions_hook, &_doActions_orig))
		DebugLog("Dialogue Extensions: Could not hook function!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&FactionManager::saveGameState), &saveGameState_hook, &saveGameState_orig))
		DebugLog("WorldStates: Could not hook function!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&GameWorld::loadAllPlatoons), &loadAllPlatoons_hook, &loadAllPlatoons_orig))
		DebugLog("WorldStates: Could not hook function!");

	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Dialogue::getSpeaker), &Dialogue_getSpeaker_hook, &Dialogue_getSpeaker_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&Dialogue::sayLine), &Dialogue_sayLine_hook, &Dialogue_sayLine_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((Item*(RootObjectFactory::*)(GameData*, const hand&, GameData*, GameData*, int, Faction*)) & RootObjectFactory::createItem), &RootObjectFactory_createItem_hook, &RootObjectFactory_createItem_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DialogLineData::_CONSTRUCTOR), &DialogLineData_CONSTRUCTOR_hook, &DialogLineData_CONSTRUCTOR_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
	// workaround for player conversations
	if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress((void(DialogLineData::*)(std::string &, bool)) & DialogLineData::getText), &DialogLineData_getText_hook, &DialogLineData_getText_orig))
		ErrorLog("Dialogue Extensions: could not install hook!");
}