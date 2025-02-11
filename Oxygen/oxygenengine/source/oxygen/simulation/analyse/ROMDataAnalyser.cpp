/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2021 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/simulation/analyse/ROMDataAnalyser.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/helper/JsonHelper.h"


ROMDataAnalyser::ROMDataAnalyser()
{
	// Load files
	loadDataFromJSONs(Configuration::instance().mAnalysisDir);
}

ROMDataAnalyser::~ROMDataAnalyser()
{
	// Save everything
	if (mAnyChange)
	{
		saveDataToJSONs(Configuration::instance().mAnalysisDir);
	}
}

bool ROMDataAnalyser::hasEntry(const std::string& categoryName, uint32 address) const
{
	return (nullptr != const_cast<ROMDataAnalyser*>(this)->findEntry(categoryName, address, false));
}

void ROMDataAnalyser::beginEntry(const std::string& categoryName, uint32 address)
{
	RMX_CHECK(nullptr == mCurrentCategory, "ROMDataAnalyser: Don't call \"beginEntry\" without closing old entry with \"endEntry\"", );
	RMX_CHECK(nullptr == mCurrentEntry,    "ROMDataAnalyser: Don't call \"beginEntry\" without closing old entry with \"endEntry\"", );
	RMX_CHECK(mCurrentObjectStack.empty(), "ROMDataAnalyser: Don't call \"beginEntry\" without closing old entry with \"endEntry\"", );

	mCurrentEntry = findEntry(categoryName, address, true, &mCurrentCategory);
	mCurrentObjectStack.clear();
	mCurrentObjectStack.push_back(&mCurrentEntry->mContent);
	mAnyChange = true;
}

void ROMDataAnalyser::endEntry()
{
	RMX_CHECK(mCurrentObjectStack.size() <= 1, "ROMDataAnalyser: Close all objects before calling \"endEntry\"", );

	mCurrentCategory = nullptr;
	mCurrentEntry = nullptr;
	mCurrentObjectStack.clear();
}

void ROMDataAnalyser::addKeyValue(const std::string& key, const std::string& value)
{
	RMX_CHECK(!mCurrentObjectStack.empty(), "ROMDataAnalyser: No current object when calling \"addKeyValue\"", return);

	mCurrentObjectStack.back()->mKeyValuePairs[key] = value;
	mAnyChange = true;
}

void ROMDataAnalyser::beginObject(const std::string& key)
{
	RMX_CHECK(!mCurrentObjectStack.empty(), "ROMDataAnalyser: No current object when calling \"beginObject\"", return);

	Object& child = mCurrentObjectStack.back()->mChildObjects[key];
	mCurrentObjectStack.push_back(&child);
	mAnyChange = true;
}

void ROMDataAnalyser::endObject()
{
	RMX_CHECK(!mCurrentObjectStack.empty(), "ROMDataAnalyser: No current object when calling \"endObject\"", return);

	mCurrentObjectStack.pop_back();
}

ROMDataAnalyser::Category* ROMDataAnalyser::findCategory(const std::string& categoryName, bool create)
{
	const uint64 hash = rmx::getMurmur2_64(categoryName);
	const auto it = mCategories.find(hash);
	if (it == mCategories.end())
	{
		if (!create)
			return nullptr;

		Category& category = mCategories[hash];
		category.mName = categoryName;
		return &category;
	}
	else
	{
		return &it->second;
	}
}

ROMDataAnalyser::Entry* ROMDataAnalyser::findEntry(const std::string& categoryName, uint32 address, bool create, Category** outCategory)
{
	Category* category = findCategory(categoryName, create);
	if (nullptr != outCategory)
		*outCategory = category;

	if (nullptr == category)
		return nullptr;

	const auto it = category->mEntries.find(address);
	if (it == category->mEntries.end())
	{
		if (!create)
			return nullptr;

		Entry& entry = category->mEntries[address];
		return &entry;
	}
	else
	{
		return &it->second;
	}
}

void ROMDataAnalyser::loadDataFromJSONs(const std::wstring& filepath)
{
	mCategories.clear();

	FileCrawler fc;
	fc.addFiles(filepath + L"romdata_*.json");
	for (size_t fileIndex = 0; fileIndex < fc.size(); ++fileIndex)
	{
		const FileCrawler::FileEntry* fileEntry = fc[fileIndex];
		if (nullptr == fileEntry)
			continue;

		const std::wstring filename = filepath + fileEntry->mFilename;
		Json::Value root = JsonHelper::loadFile(filename);
		if (root.isNull())
			continue;

		for (Json::ValueConstIterator it = root.begin(); it != root.end(); ++it)
		{
			for (Json::ValueConstIterator it2 = it->begin(); it2 != it->end(); ++it2)
			{
				const uint32 address = (uint32)rmx::parseInteger(String(it2.key().asCString()));
				Object& object = findEntry(it.key().asCString(), address, true)->mContent;
				recursiveLoadDataFromJSON(*it2, object);
			}
		}
	}
}

void ROMDataAnalyser::recursiveLoadDataFromJSON(const Json::Value& json, Object& outObject)
{
	for (Json::ValueConstIterator it = json.begin(); it != json.end(); ++it)
	{
		if (it->isObject())
		{
			Object& child = outObject.mChildObjects[it.key().asCString()];
			recursiveLoadDataFromJSON(*it, child);
		}
		else
		{
			outObject.mKeyValuePairs[it.key().asCString()] = it->asCString();
		}
	}
}

void ROMDataAnalyser::saveDataToJSONs(const std::wstring& filepath)
{
	for (const auto& pair : mCategories)
	{
		Json::Value root;

		const Category& category = pair.second;
		Json::Value categoryJson;
		for (const auto& pair2 : category.mEntries)
		{
			const Entry& entry = pair2.second;
			Json::Value entryJson;
			recursiveSaveDataToJSON(entryJson, entry.mContent);
			categoryJson[rmx::hexString(pair2.first, 6)] = entryJson;
		}
		root[category.mName] = categoryJson;

		// Save file
		const std::wstring filename = filepath + L"romdata_" + *String(category.mName).toWString() + L".json";
		JsonHelper::saveFile(filename, root);

	#if 0
		if (category.mName == "TableLookup")
		{
			String output;
			for (const auto& pair2 : category.mEntries)
			{
				output << rmx::hexString(pair2.first, 6) << "\r\n";
				output << "\t// Targets:\r\n";
				for (const auto& pair3 : pair2.second.mContent.mKeyValuePairs)
				{
					const int value = (int)rmx::parseInteger(pair3.first);
					output << "\t//  - " << pair3.second << "\t-> objA0.base_state = " << rmx::hexString(value, 2) << "\r\n";
				}
				output << "\r\n";
			}
			output.saveFile("TableLookup.txt");
		}
	#endif
	}

	mAnyChange = false;
}

void ROMDataAnalyser::recursiveSaveDataToJSON(Json::Value& outJson, const Object& object)
{
	for (const auto& pair : object.mKeyValuePairs)
	{
		outJson[pair.first] = pair.second;
	}
	for (const auto& pair : object.mChildObjects)
	{
		Json::Value childJson;
		recursiveSaveDataToJSON(childJson, pair.second);
		outJson[pair.first] = childJson;
	}
}
