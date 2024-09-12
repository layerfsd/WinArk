#include "stdafx.h"
#include "LocationManager.h"
#include "IniFile.h"

bool LocationManager::LoadFromRegistry(PCWSTR path) {
	path = GetPath(path);
	if (!path)
		return false;

	CRegKey key;
	key.Open(HKEY_CURRENT_USER, path, KEY_QUERY_VALUE);
	if (!key)
		return false;

	ULONG chars = 0;
	key.QueryStringValue(L"Locations", nullptr, &chars);
	if (chars == 0)
		return false;

	auto data = std::make_unique<WCHAR[]>(chars);
	if (ERROR_SUCCESS != key.QueryStringValue(L"Locations", data.get(), &chars))
		return false;

	_items.clear();
	for (auto p = data.get(); *p;) {
		auto semi = wcschr(p, L';');
		if (semi == nullptr)
			return false;

		auto count = semi - p;
		CString name((PCWSTR)p, (int)count);
		auto cr = wcschr(semi + 1, L'\n');
		CString path(semi + 1, (int)(cr - semi - 1));
		if (name.IsEmpty())
			name = path;
		_items.insert({ name,path });
		p = cr + 1;
	}

	return true;
}

bool LocationManager::SaveToRegistry(PCWSTR path) const {
	if (_items.empty())
		return true;

	path = GetPath(path);
	if (!path)
		return false;

	CRegKey key;
	key.Create(HKEY_CURRENT_USER, path, nullptr, 0, KEY_WRITE);
	if (!key)
		return false;

	CString text;
	for (const auto& [name, value] : _items) {
		text += name + L":" + value + L"\n";
	}

	return ERROR_SUCCESS == key.SetStringValue(L"Locations", text);
}

bool LocationManager::LoadFromFile(PCWSTR path) {
	path = GetPath(path);
	if (!path)
		return false;

	IniFile file(path);
	if (!file.IsValid())
		return false;

	auto data = file.ReadSection(L"Location");
	for (auto& text : data) {
		int n = text.Find(L'=');
		_items.insert({ n < 0 ? text : text.Left(n),n < 0 ? text : text.Mid(n + 1) });
	}

	return true;
}

bool LocationManager::SaveToFile(PCWSTR path) const {
	path = GetPath(path);
	if (!path)
		return false;

	IniFile file(path);
	for (auto& [name, target] : _items) {
		file.WriteString(L"Location", name, target);
	}
	return true;
}

bool LocationManager::Load(PCWSTR path) {
	WCHAR fullpath[MAX_PATH];
	::GetModuleFileName(nullptr, fullpath, _countof(fullpath));
	auto dot = wcsrchr(fullpath, L'.');
	if (!dot)
		return false;

	*dot = 0;
	wcscat_s(fullpath, L".ini");

	if (::GetFileAttributes(fullpath) == INVALID_FILE_ATTRIBUTES) {
		return LoadFromRegistry(path);
	}

	// load from file
	return LoadFromFile(fullpath);
}

bool LocationManager::Save() const {
	if (_path.IsEmpty())
		return false;

	return _path[1] == L':' ? SaveToFile() : SaveToRegistry();
}

int LocationManager::GetCount() const {
	return (int)_items.size();
}

bool LocationManager::Replace(CString const& name, CString const& newName) {
	auto it = _items.find(name);
	if (it == _items.end())
		return false;

	auto path = it->second;
	_items.erase(it);
	_items.insert({ newName,path });
	return true;
}

void LocationManager::Add(CString const& name, CString const& path) {
	_items.insert({ name,path });
}

void LocationManager::Clear() {
	_items.clear();
}

CString const& LocationManager::GetPathByName(CString const& name) const {
	auto& path = _items.at(name);
	return path.IsEmpty() ? name : path;
}

PCWSTR LocationManager::GetPath(PCWSTR path) const {
	if (path == nullptr || *path == 0)
		path = _path;

	_path = path;
	if (path == nullptr || *path == 0)
		return nullptr;

	return path;
}