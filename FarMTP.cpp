/*
  FarMTP — плагин для Far Manager 2.0 (Unicode, x64/x86).
  Доступ к телефонам/планшетам Android и другим MTP-устройствам
  через Windows Portable Devices (WPD) — тот же механизм, что у Проводника.
  На телефон ничего ставить не нужно.

  Возможности: просмотр, копирование/перемещение с устройства и на устройство
  (F5/F6, рекурсивно), удаление (F8), создание папок (F7),
  переименование (Shift+F6), просмотр/редактирование (F3/F4).
*/

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <oaidl.h>
#include <ocidl.h>
#include <propidl.h>
#include <propsys.h>

#include <initguid.h>
#include "portabledeviceapi.h"
#include "portabledevice.h"

#include "plugin.hpp"

#include <string>
#include <vector>
#include <map>
#include <set>

#ifndef PORTABLE_DEVICE_DELETE_NO_RECURSION
#define PORTABLE_DEVICE_DELETE_NO_RECURSION   0
#define PORTABLE_DEVICE_DELETE_WITH_RECURSION 1
#endif

using std::wstring;
using std::vector;

static struct PluginStartupInfo Info;
static FARSTANDARDFUNCTIONS FSF;

static const wchar_t *const PLUGIN_TITLE = L"MTP";
static const wchar_t *const ROOT_ID = WPD_DEVICE_OBJECT_ID; // L"DEVICE"

/* ------------------------------------------------------------------------ */
/*  Мелкие утилиты                                                           */
/* ------------------------------------------------------------------------ */

template <class T> static void SafeRelease(T *&p)
{
	if (p) { p->Release(); p = nullptr; }
}

static wstring HrText(HRESULT hr)
{
	wchar_t *buf = nullptr;
	DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
	                         FORMAT_MESSAGE_IGNORE_INSERTS,
	                         nullptr, (DWORD)hr, 0, (LPWSTR)&buf, 0, nullptr);
	wstring s;
	if (n && buf)
	{
		s = buf;
		while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.'))
			s.pop_back();
	}
	if (buf) LocalFree(buf);
	wchar_t code[32];
	wsprintfW(code, L"0x%08X", (unsigned)hr);
	if (s.empty()) return wstring(L"Ошибка ") + code;
	return s + L" (" + code + L")";
}

static bool NameEq(const wstring &a, const wstring &b)
{
	return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a.c_str(), (int)a.size(),
	                      b.c_str(), (int)b.size()) == CSTR_EQUAL;
}

static wstring Lower(const wstring &s)
{
	wstring r = s;
	if (!r.empty()) CharLowerBuffW(&r[0], (DWORD)r.size());
	return r;
}

static wchar_t *DupStr(const wstring &s)
{
	wchar_t *p = new wchar_t[s.size() + 1];
	memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
	return p;
}

// Имя с устройства может содержать символы, недопустимые в Windows.
static wstring SafeLocalName(const wstring &name)
{
	wstring r;
	for (wchar_t c : name)
	{
		if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) r += L'_';
		else r += c;
	}
	while (!r.empty() && (r.back() == L'.' || r.back() == L' ')) r.pop_back();
	if (r.empty()) r = L"_";
	return r;
}

static wstring JoinPath(const wstring &dir, const wstring &name)
{
	if (dir.empty()) return name;
	if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
	return dir + L"\\" + name;
}

static bool IsAbsLocalPath(const wstring &p)
{
	return (p.size() > 1 && p[1] == L':') || (p.size() > 1 && p[0] == L'\\' && p[1] == L'\\');
}

static bool ForceDirectories(const wstring &path)
{
	if (path.empty()) return false;
	DWORD a = GetFileAttributesW(path.c_str());
	if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
	size_t pos = path.find_last_of(L"\\/");
	if (pos != wstring::npos && pos > 0)
	{
		wstring parent = path.substr(0, pos);
		if (!(parent.size() == 2 && parent[1] == L':')) ForceDirectories(parent);
	}
	return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static wstring SizeText(unsigned __int64 v)
{
	wchar_t b[64];
	if (v < 1024ull * 10) wsprintfW(b, L"%u Б", (unsigned)v);
	else if (v < 1024ull * 1024 * 10) wsprintfW(b, L"%u КБ", (unsigned)(v / 1024));
	else if (v < 1024ull * 1024 * 1024 * 10) wsprintfW(b, L"%u МБ", (unsigned)(v / (1024 * 1024)));
	else wsprintfW(b, L"%u ГБ", (unsigned)(v / (1024ull * 1024 * 1024)));
	return b;
}

static wstring Truncate(const wstring &s, size_t max)
{
	if (s.size() <= max) return s;
	return L"..." + s.substr(s.size() - (max - 3));
}

/* ------------------------------------------------------------------------ */
/*  Сообщения Far                                                            */
/* ------------------------------------------------------------------------ */

static int FarMsg(DWORD flags, const vector<wstring> &lines, int buttons)
{
	vector<const wchar_t *> items;
	for (auto &l : lines) items.push_back(l.c_str());
	return Info.Message(Info.ModuleNumber, flags, nullptr, items.data(), (int)items.size(), buttons);
}

static void ShowError(const wstring &what, const wstring &obj, HRESULT hr)
{
	vector<wstring> l;
	l.push_back(PLUGIN_TITLE);
	l.push_back(what);
	if (!obj.empty()) l.push_back(Truncate(obj, 60));
	if (hr != S_OK) l.push_back(HrText(hr));
	FarMsg(FMSG_WARNING | FMSG_MB_OK, l, 0);
}

static void ShowInfo(const wstring &text)
{
	vector<wstring> l;
	l.push_back(PLUGIN_TITLE);
	l.push_back(text);
	FarMsg(FMSG_MB_OK, l, 0);
}

static bool EscPressed()
{
	HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
	DWORD n = 0;
	bool esc = false;
	while (GetNumberOfConsoleInputEvents(hIn, &n) && n)
	{
		INPUT_RECORD rec;
		DWORD read = 0;
		if (!ReadConsoleInputW(hIn, &rec, 1, &read) || !read) break;
		if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown &&
		    rec.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE)
			esc = true;
	}
	return esc;
}

static bool ConfirmAbort()
{
	vector<wstring> l = { PLUGIN_TITLE, L"Прервать операцию?", L"Да", L"Нет" };
	return FarMsg(FMSG_WARNING, l, 2) == 0;
}

/* ------------------------------------------------------------------------ */
/*  Работа с WPD                                                             */
/* ------------------------------------------------------------------------ */

struct Entry
{
	wstring id;
	wstring name;
	bool dir = false;
	unsigned __int64 size = 0;
	FILETIME mtime = {0, 0};
};

static void FillEntry(IPortableDeviceValues *v, Entry &e)
{
	LPWSTR s = nullptr;
	if (SUCCEEDED(v->GetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, &s)) && s && *s)
		e.name = s;
	if (s) { CoTaskMemFree(s); s = nullptr; }
	if (e.name.empty())
	{
		if (SUCCEEDED(v->GetStringValue(WPD_OBJECT_NAME, &s)) && s && *s) e.name = s;
		if (s) { CoTaskMemFree(s); s = nullptr; }
	}
	if (e.name.empty()) e.name = e.id;

	// Имена с '\' сломали бы навигацию по путям Far
	for (auto &c : e.name) if (c == L'\\' || c == L'/') c = L'_';

	GUID ct;
	if (SUCCEEDED(v->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &ct)))
		e.dir = IsEqualGUID(ct, WPD_CONTENT_TYPE_FOLDER) || IsEqualGUID(ct, WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT);

	ULONGLONG sz = 0;
	if (!e.dir && SUCCEEDED(v->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, &sz)))
		e.size = sz;

	PROPVARIANT pv;
	PropVariantInit(&pv);
	if (SUCCEEDED(v->GetValue(WPD_OBJECT_DATE_MODIFIED, &pv)) && pv.vt == VT_DATE)
	{
		SYSTEMTIME st;
		FILETIME lt;
		if (VariantTimeToSystemTime(pv.date, &st) && SystemTimeToFileTime(&st, &lt))
			LocalFileTimeToFileTime(&lt, &e.mtime);
	}
	PropVariantClear(&pv);
}

// Callback для пакетного чтения свойств (быстро на больших папках)
class BulkCallback : public IPortableDevicePropertiesBulkCallback
{
	LONG m_ref = 1;
public:
	HANDLE done;
	HRESULT status = S_OK;
	CRITICAL_SECTION cs;
	vector<Entry> results;

	BulkCallback()
	{
		done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		InitializeCriticalSection(&cs);
	}
	virtual ~BulkCallback()
	{
		CloseHandle(done);
		DeleteCriticalSection(&cs);
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv) return E_POINTER;
		if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPortableDevicePropertiesBulkCallback))
		{
			*ppv = static_cast<IPortableDevicePropertiesBulkCallback *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (!r) delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE OnStart(REFGUID) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE OnProgress(REFGUID, IPortableDeviceValuesCollection *coll) override
	{
		if (!coll) return S_OK;
		DWORD n = 0;
		coll->GetCount(&n);
		for (DWORD i = 0; i < n; i++)
		{
			IPortableDeviceValues *v = nullptr;
			if (FAILED(coll->GetAt(i, &v)) || !v) continue;
			Entry e;
			LPWSTR id = nullptr;
			if (SUCCEEDED(v->GetStringValue(WPD_OBJECT_ID, &id)) && id)
			{
				e.id = id;
				CoTaskMemFree(id);
				FillEntry(v, e);
				EnterCriticalSection(&cs);
				results.push_back(e);
				LeaveCriticalSection(&cs);
			}
			v->Release();
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE OnEnd(REFGUID, HRESULT hr) override
	{
		status = hr;
		SetEvent(done);
		return S_OK;
	}
};

static IPortableDeviceKeyCollection *MakeKeys()
{
	IPortableDeviceKeyCollection *keys = nullptr;
	if (FAILED(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER,
	                            IID_IPortableDeviceKeyCollection, (void **)&keys)))
		return nullptr;
	keys->Add(WPD_OBJECT_ID);
	keys->Add(WPD_OBJECT_NAME);
	keys->Add(WPD_OBJECT_ORIGINAL_FILE_NAME);
	keys->Add(WPD_OBJECT_CONTENT_TYPE);
	keys->Add(WPD_OBJECT_SIZE);
	keys->Add(WPD_OBJECT_DATE_MODIFIED);
	return keys;
}

static IPortableDevicePropVariantCollection *MakeIdCollection(const vector<wstring> &ids)
{
	IPortableDevicePropVariantCollection *c = nullptr;
	if (FAILED(CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr, CLSCTX_INPROC_SERVER,
	                            IID_IPortableDevicePropVariantCollection, (void **)&c)))
		return nullptr;
	for (auto &id : ids)
	{
		PROPVARIANT pv;
		PropVariantInit(&pv);
		pv.vt = VT_LPWSTR;
		pv.pwszVal = (LPWSTR)CoTaskMemAlloc((id.size() + 1) * sizeof(wchar_t));
		if (pv.pwszVal)
		{
			memcpy(pv.pwszVal, id.c_str(), (id.size() + 1) * sizeof(wchar_t));
			c->Add(&pv);
		}
		PropVariantClear(&pv);
	}
	return c;
}

struct Device
{
	IPortableDevice *dev = nullptr;
	IPortableDeviceContent *content = nullptr;
	IPortableDeviceProperties *props = nullptr;
	IPortableDeviceResources *res = nullptr;
	IPortableDevicePropertiesBulk *bulk = nullptr;
	bool readOnly = false;

	bool IsOpen() const { return dev != nullptr; }

	void Close()
	{
		SafeRelease(bulk);
		SafeRelease(res);
		SafeRelease(props);
		SafeRelease(content);
		if (dev) { dev->Close(); SafeRelease(dev); }
		readOnly = false;
	}

	HRESULT Open(const wstring &deviceId)
	{
		Close();
		IPortableDeviceValues *client = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
		                              IID_IPortableDeviceValues, (void **)&client);
		if (FAILED(hr)) return hr;
		client->SetStringValue(WPD_CLIENT_NAME, L"FarMTP");
		client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
		client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
		client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
		client->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION);

		hr = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER,
		                      IID_IPortableDevice, (void **)&dev);
		if (SUCCEEDED(hr))
		{
			hr = dev->Open(deviceId.c_str(), client);
			if (hr == E_ACCESSDENIED)
			{
				// Нет прав на запись — пробуем только чтение
				client->SetUnsignedIntegerValue(WPD_CLIENT_DESIRED_ACCESS, GENERIC_READ);
				hr = dev->Open(deviceId.c_str(), client);
				if (SUCCEEDED(hr)) readOnly = true;
			}
		}
		client->Release();
		if (SUCCEEDED(hr)) hr = dev->Content(&content);
		if (SUCCEEDED(hr)) hr = content->Properties(&props);
		if (SUCCEEDED(hr)) hr = content->Transfer(&res);
		if (SUCCEEDED(hr))
		{
			if (FAILED(props->QueryInterface(IID_IPortableDevicePropertiesBulk, (void **)&bulk)))
				bulk = nullptr;
		}
		if (FAILED(hr)) Close();
		return hr;
	}

	HRESULT EnumIds(const wstring &parent, vector<wstring> &ids)
	{
		ids.clear();
		IEnumPortableDeviceObjectIDs *en = nullptr;
		HRESULT hr = content->EnumObjects(0, parent.c_str(), nullptr, &en);
		if (FAILED(hr)) return hr;
		for (;;)
		{
			LPWSTR buf[256];
			DWORD got = 0;
			hr = en->Next(256, buf, &got);
			for (DWORD i = 0; i < got; i++)
			{
				if (buf[i]) { ids.push_back(buf[i]); CoTaskMemFree(buf[i]); }
			}
			if (FAILED(hr) || hr == S_FALSE || got == 0) break;
		}
		en->Release();
		return FAILED(hr) ? hr : S_OK;
	}

	HRESULT ReadOne(const wstring &id, IPortableDeviceKeyCollection *keys, Entry &e)
	{
		IPortableDeviceValues *v = nullptr;
		HRESULT hr = props->GetValues(id.c_str(), keys, &v);
		if (FAILED(hr) || !v) return FAILED(hr) ? hr : E_FAIL;
		e.id = id;
		FillEntry(v, e);
		v->Release();
		return S_OK;
	}

	bool ReadBulk(const vector<wstring> &ids, IPortableDeviceKeyCollection *keys, vector<Entry> &out)
	{
		if (!bulk) return false;
		IPortableDevicePropVariantCollection *coll = MakeIdCollection(ids);
		if (!coll) return false;
		BulkCallback *cb = new BulkCallback();
		GUID ctx = GUID_NULL;
		HRESULT hr = bulk->QueueGetValuesByObjectList(coll, keys, cb, &ctx);
		coll->Release();
		if (SUCCEEDED(hr)) hr = bulk->Start(ctx);
		bool ok = false;
		if (SUCCEEDED(hr))
		{
			DWORD started = GetTickCount();
			for (;;)
			{
				DWORD idx = 0;
				HRESULT w = CoWaitForMultipleHandles(0, 250, 1, &cb->done, &idx);
				if (w == S_OK) break;
				// Драйвер завис на пакетном запросе — отменяем, дальше читаем поштучно
				if (GetTickCount() - started > 60000 || (EscPressed() && ConfirmAbort()))
				{
					bulk->Cancel(ctx);
					WaitForSingleObject(cb->done, 5000);
					break;
				}
			}
			if (WaitForSingleObject(cb->done, 0) == WAIT_OBJECT_0 && SUCCEEDED(cb->status))
			{
				EnterCriticalSection(&cb->cs);
				out = cb->results;
				LeaveCriticalSection(&cb->cs);
				ok = true;
			}
		}
		cb->Release();
		return ok;
	}

	HRESULT List(const wstring &parent, vector<Entry> &out)
	{
		out.clear();
		vector<wstring> ids;
		HRESULT hr = EnumIds(parent, ids);
		if (FAILED(hr)) return hr;
		if (ids.empty()) return S_OK;

		IPortableDeviceKeyCollection *keys = MakeKeys();
		if (!keys) return E_OUTOFMEMORY;

		std::set<wstring> got;
		if (ids.size() > 4 && ReadBulk(ids, keys, out))
			for (auto &e : out) got.insert(e.id);

		for (auto &id : ids)
		{
			if (got.count(id)) continue;
			Entry e;
			if (SUCCEEDED(ReadOne(id, keys, e))) out.push_back(e);
		}
		keys->Release();
		return S_OK;
	}

	HRESULT Delete(const wstring &id, bool isDir)
	{
		vector<wstring> one(1, id);
		IPortableDevicePropVariantCollection *c = MakeIdCollection(one);
		if (!c) return E_OUTOFMEMORY;
		HRESULT hr = content->Delete(isDir ? PORTABLE_DEVICE_DELETE_WITH_RECURSION
		                                   : PORTABLE_DEVICE_DELETE_NO_RECURSION, c, nullptr);
		c->Release();
		if (FAILED(hr) && isDir)
		{
			// Устройство не умеет удалять рекурсивно — удаляем содержимое вручную
			vector<Entry> kids;
			if (SUCCEEDED(List(id, kids)))
			{
				for (auto &k : kids)
				{
					HRESULT h2 = Delete(k.id, k.dir);
					if (FAILED(h2)) return h2;
				}
				c = MakeIdCollection(one);
				if (!c) return E_OUTOFMEMORY;
				hr = content->Delete(PORTABLE_DEVICE_DELETE_NO_RECURSION, c, nullptr);
				c->Release();
			}
		}
		return hr;
	}

	HRESULT CreateFolder(const wstring &parent, const wstring &name, wstring &newId)
	{
		IPortableDeviceValues *v = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
		                              IID_IPortableDeviceValues, (void **)&v);
		if (FAILED(hr)) return hr;
		v->SetStringValue(WPD_OBJECT_PARENT_ID, parent.c_str());
		v->SetStringValue(WPD_OBJECT_NAME, name.c_str());
		v->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, name.c_str());
		v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, WPD_CONTENT_TYPE_FOLDER);
		LPWSTR id = nullptr;
		hr = content->CreateObjectWithPropertiesOnly(v, &id);
		v->Release();
		if (SUCCEEDED(hr) && id) { newId = id; CoTaskMemFree(id); }
		return hr;
	}

	HRESULT Rename(const wstring &id, const wstring &newName)
	{
		IPortableDeviceValues *v = nullptr, *res = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
		                              IID_IPortableDeviceValues, (void **)&v);
		if (FAILED(hr)) return hr;
		v->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, newName.c_str());
		hr = props->SetValues(id.c_str(), v, &res);
		HRESULT perProp = S_OK;
		if (res)
		{
			if (FAILED(res->GetErrorValue(WPD_OBJECT_ORIGINAL_FILE_NAME, &perProp))) perProp = S_OK;
			SafeRelease(res);
		}
		v->Release();
		if (SUCCEEDED(hr) && FAILED(perProp)) hr = perProp;
		if (FAILED(hr))
		{
			// Запасной вариант — через WPD_OBJECT_NAME
			if (SUCCEEDED(CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
			                               IID_IPortableDeviceValues, (void **)&v)))
			{
				v->SetStringValue(WPD_OBJECT_NAME, newName.c_str());
				HRESULT h2 = props->SetValues(id.c_str(), v, &res);
				HRESULT p2 = S_OK;
				if (res)
				{
					if (FAILED(res->GetErrorValue(WPD_OBJECT_NAME, &p2))) p2 = S_OK;
					SafeRelease(res);
				}
				v->Release();
				if (SUCCEEDED(h2) && SUCCEEDED(p2)) hr = S_OK;
			}
		}
		return hr;
	}
};

struct DevInfo
{
	wstring id;
	wstring name;
};

static void EnumDevices(vector<DevInfo> &out)
{
	out.clear();
	IPortableDeviceManager *mgr = nullptr;
	if (FAILED(CoCreateInstance(CLSID_PortableDeviceManager, nullptr, CLSCTX_INPROC_SERVER,
	                            IID_IPortableDeviceManager, (void **)&mgr)))
		return;
	mgr->RefreshDeviceList();
	DWORD count = 0;
	if (SUCCEEDED(mgr->GetDevices(nullptr, &count)) && count)
	{
		vector<LPWSTR> ids(count, nullptr);
		if (SUCCEEDED(mgr->GetDevices(ids.data(), &count)))
		{
			std::map<wstring, int> used;
			for (DWORD i = 0; i < count; i++)
			{
				if (!ids[i]) continue;
				DevInfo d;
				d.id = ids[i];
				DWORD len = 0;
				if (SUCCEEDED(mgr->GetDeviceFriendlyName(ids[i], nullptr, &len)) && len)
				{
					vector<wchar_t> b(len + 1, 0);
					if (SUCCEEDED(mgr->GetDeviceFriendlyName(ids[i], b.data(), &len))) d.name = b.data();
				}
				if (d.name.empty())
				{
					len = 0;
					if (SUCCEEDED(mgr->GetDeviceDescription(ids[i], nullptr, &len)) && len)
					{
						vector<wchar_t> b(len + 1, 0);
						if (SUCCEEDED(mgr->GetDeviceDescription(ids[i], b.data(), &len))) d.name = b.data();
					}
				}
				if (d.name.empty()) d.name = L"Устройство";
				for (auto &c : d.name) if (c == L'\\' || c == L'/') c = L'_';
				int n = ++used[Lower(d.name)];
				if (n > 1)
				{
					wchar_t suf[16];
					wsprintfW(suf, L" (%d)", n);
					d.name += suf;
				}
				out.push_back(d);
				CoTaskMemFree(ids[i]);
			}
		}
	}
	mgr->Release();
}

/* ------------------------------------------------------------------------ */
/*  Прогресс и параметры операции копирования                                */
/* ------------------------------------------------------------------------ */

enum OverwritePolicy { OW_ASK, OW_ALL, OW_SKIPALL };

struct Op
{
	bool silent = false;
	bool move = false;
	bool cancelled = false;
	OverwritePolicy policy = OW_ASK;
	const wchar_t *title = L"Копирование";
	HANDLE screen = nullptr;
	DWORD lastTick = 0;
	int filesDone = 0;
	unsigned __int64 bytesDone = 0;

	void Begin()
	{
		if (!silent) screen = Info.SaveScreen(0, 0, -1, -1);
	}
	void End()
	{
		if (screen) { Info.RestoreScreen(screen); screen = nullptr; }
	}

	void Progress(const wstring &from, const wstring &to, unsigned __int64 cur, unsigned __int64 total, bool force = false)
	{
		if (silent) return;
		DWORD t = GetTickCount();
		if (!force && t - lastTick < 200) return;
		lastTick = t;
		const int W = 50;
		int fill = total ? (int)((cur * W) / total) : W;
		if (fill > W) fill = W;
		wstring bar(fill, L'\x2588');
		bar += wstring(W - fill, L'\x2591');
		wchar_t pct[16];
		wsprintfW(pct, L" %3d%%", total ? (int)((cur * 100) / total) : 100);
		wchar_t cnt[96];
		wsprintfW(cnt, L"Готово файлов: %d", filesDone);
		vector<wstring> l;
		l.push_back(title);
		l.push_back(Truncate(from, W + 5));
		l.push_back(L"\x2192 " + Truncate(to, W + 3));
		l.push_back(bar + pct);
		l.push_back(SizeText(cur) + L" из " + SizeText(total) + L"   " + cnt);
		FarMsg(FMSG_LEFTALIGN, l, 0);
	}

	bool CheckCancel()
	{
		if (cancelled) return true;
		if (!silent && EscPressed() && ConfirmAbort()) cancelled = true;
		return cancelled;
	}

	// true — перезаписывать, false — пропустить. Может выставить cancelled.
	bool AskOverwrite(const wstring &name, const wchar_t *where)
	{
		if (silent || policy == OW_ALL) return true;
		if (policy == OW_SKIPALL) return false;
		vector<wstring> l = { L"Предупреждение", wstring(L"Объект уже существует ") + where + L":",
		                      Truncate(name, 60),
		                      L"Заменить", L"Заменить все", L"Пропустить", L"Пропустить все", L"Отмена" };
		int r = FarMsg(FMSG_WARNING, l, 5);
		lastTick = 0;
		switch (r)
		{
			case 0: return true;
			case 1: policy = OW_ALL; return true;
			case 2: return false;
			case 3: policy = OW_SKIPALL; return false;
			default: cancelled = true; return false;
		}
	}

	// Ошибка во время пакетной операции: true — продолжить, false — прервать
	bool ErrorContinue(const wstring &what, const wstring &obj, HRESULT hr)
	{
		if (silent) { cancelled = true; return false; }
		vector<wstring> l = { L"Ошибка", what, Truncate(obj, 60), HrText(hr), L"Пропустить", L"Отмена" };
		int r = FarMsg(FMSG_WARNING, l, 2);
		lastTick = 0;
		if (r != 0) cancelled = true;
		return r == 0;
	}
};

/* ------------------------------------------------------------------------ */
/*  Панель плагина                                                           */
/* ------------------------------------------------------------------------ */

struct PathItem
{
	wstring id;
	wstring name;
};

struct Panel
{
	bool comInit = false;
	vector<DevInfo> devs;
	int devIdx = -1;          // -1 — список устройств
	wstring devName;
	Device dev;
	vector<PathItem> path;    // путь внутри устройства (хранилище, папки)
	wstring curDir, title;
	vector<Entry> cache;      // последний список текущей папки
	wstring cacheParent;

	bool AtRoot() const { return devIdx < 0; }
	wstring CurId() const { return path.empty() ? wstring(ROOT_ID) : path.back().id; }

	void UpdateStrings()
	{
		curDir.clear();
		if (!AtRoot())
		{
			curDir = devName;
			for (auto &p : path) curDir += L"\\" + p.name;
		}
		title = wstring(L" ") + PLUGIN_TITLE + L":\\" + curDir + L" ";
	}

	void GoRoot()
	{
		dev.Close();
		devIdx = -1;
		devName.clear();
		path.clear();
		cache.clear();
		cacheParent.clear();
	}

	HRESULT ListCurrent(vector<Entry> &out)
	{
		HRESULT hr = dev.List(CurId(), out);
		if (SUCCEEDED(hr)) { cache = out; cacheParent = CurId(); }
		return hr;
	}

	bool FindChild(const wstring &name, Entry &e, bool refresh = false)
	{
		if (refresh || cacheParent != CurId())
		{
			vector<Entry> tmp;
			if (FAILED(ListCurrent(tmp))) return false;
		}
		for (auto &c : cache)
			if (NameEq(c.name, name)) { e = c; return true; }
		if (!refresh) return FindChild(name, e, true);
		return false;
	}

	bool EnterDevice(const wstring &name, bool quiet)
	{
		bool found = false;
		for (auto &d : devs) if (NameEq(d.name, name)) found = true;
		if (!found) EnumDevices(devs);
		for (size_t i = 0; i < devs.size(); i++)
		{
			if (!NameEq(devs[i].name, name)) continue;
			HRESULT hr = dev.Open(devs[i].id);
			if (FAILED(hr))
			{
				if (!quiet)
					ShowError(L"Не удалось открыть устройство", devs[i].name +
					          L" — разблокируйте телефон и выберите режим «Передача файлов»", hr);
				return false;
			}
			devIdx = (int)i;
			devName = devs[i].name;
			path.clear();
			cache.clear();
			cacheParent.clear();
			return true;
		}
		return false;
	}

	bool Step(const wstring &comp, bool quiet)
	{
		if (comp.empty() || comp == L".") return true;
		if (comp == L"..")
		{
			if (!path.empty()) { path.pop_back(); return true; }
			if (!AtRoot()) { GoRoot(); return true; }
			return false;
		}
		if (AtRoot()) return EnterDevice(comp, quiet);
		Entry e;
		if (!FindChild(comp, e) || !e.dir) return false;
		PathItem p;
		p.id = e.id;
		p.name = e.name;
		path.push_back(p);
		return true;
	}

	bool SetDir(const wstring &dirIn, bool quiet)
	{
		wstring d = dirIn;
		if (d.empty()) return true;
		// Сохраняем состояние на случай неудачи
		int oldIdx = devIdx;
		wstring oldName = devName;
		vector<PathItem> oldPath = path;

		if (d[0] == L'\\' || d[0] == L'/')
		{
			GoRoot();
			d.erase(0, 1);
		}
		size_t start = 0;
		bool ok = true;
		while (start <= d.size())
		{
			size_t pos = d.find_first_of(L"\\/", start);
			wstring comp = d.substr(start, pos == wstring::npos ? wstring::npos : pos - start);
			if (!Step(comp, quiet)) { ok = false; break; }
			if (pos == wstring::npos) break;
			start = pos + 1;
		}
		if (!ok)
		{
			// Откат
			if (devIdx != oldIdx)
			{
				GoRoot();
				if (oldIdx >= 0 && oldIdx < (int)devs.size()) EnterDevice(oldName, true);
			}
			path = oldPath;
			cache.clear();
			cacheParent.clear();
		}
		UpdateStrings();
		return ok;
	}
};

/* ------------------------------------------------------------------------ */
/*  Копирование с устройства                                                 */
/* ------------------------------------------------------------------------ */

static bool DownloadFile(Panel *p, Op &op, const Entry &e, const wstring &destDir)
{
	wstring target = JoinPath(destDir, SafeLocalName(e.name));
	DWORD attr = GetFileAttributesW(target.c_str());
	if (attr != INVALID_FILE_ATTRIBUTES)
	{
		if (attr & FILE_ATTRIBUTE_DIRECTORY)
			return op.ErrorContinue(L"На месте файла уже есть папка", target, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
		if (!op.AskOverwrite(target, L"на диске")) return !op.cancelled;
		if (attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
			SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
	}

	IStream *s = nullptr;
	DWORD opt = 0;
	HRESULT hr = p->dev.res->GetStream(e.id.c_str(), WPD_RESOURCE_DEFAULT, STGM_READ, &opt, &s);
	if (FAILED(hr)) return op.ErrorContinue(L"Не удалось прочитать файл с устройства", e.name, hr);

	HANDLE f = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
	{
		DWORD err = GetLastError();
		s->Release();
		return op.ErrorContinue(L"Не удалось создать файл", target, HRESULT_FROM_WIN32(err));
	}

	DWORD bufSize = opt ? opt : 256 * 1024;
	if (bufSize < 64 * 1024) bufSize = 64 * 1024;
	if (bufSize > 4 * 1024 * 1024) bufSize = 4 * 1024 * 1024;
	vector<BYTE> buf(bufSize);
	unsigned __int64 done = 0;
	bool ok = true;
	op.Progress(e.name, target, 0, e.size, true);
	for (;;)
	{
		ULONG got = 0;
		hr = s->Read(buf.data(), bufSize, &got);
		if (FAILED(hr)) { ok = false; break; }
		if (got == 0) break;
		DWORD wr = 0;
		if (!WriteFile(f, buf.data(), got, &wr, nullptr) || wr != got)
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			ok = false;
			break;
		}
		done += got;
		op.bytesDone += got;
		op.Progress(e.name, target, done, e.size);
		if (op.CheckCancel()) { ok = false; hr = S_OK; break; }
		if (hr == S_FALSE) break;
	}
	if (ok && (e.mtime.dwLowDateTime || e.mtime.dwHighDateTime))
		SetFileTime(f, nullptr, nullptr, &e.mtime);
	CloseHandle(f);
	s->Release();

	if (!ok)
	{
		DeleteFileW(target.c_str());
		if (op.cancelled) return false;
		return op.ErrorContinue(L"Ошибка при копировании файла", e.name, hr);
	}
	op.filesDone++;
	if (op.move)
	{
		HRESULT h2 = p->dev.Delete(e.id, false);
		if (FAILED(h2)) return op.ErrorContinue(L"Файл скопирован, но не удалён с устройства", e.name, h2);
	}
	return true;
}

static bool DownloadDir(Panel *p, Op &op, const Entry &e, const wstring &destDir)
{
	wstring target = JoinPath(destDir, SafeLocalName(e.name));
	if (!ForceDirectories(target))
		return op.ErrorContinue(L"Не удалось создать папку", target, HRESULT_FROM_WIN32(GetLastError()));

	vector<Entry> kids;
	HRESULT hr = p->dev.List(e.id, kids);
	if (FAILED(hr)) return op.ErrorContinue(L"Не удалось прочитать папку на устройстве", e.name, hr);

	bool allOk = true;
	for (auto &k : kids)
	{
		if (op.CheckCancel()) return false;
		bool r = k.dir ? DownloadDir(p, op, k, target) : DownloadFile(p, op, k, target);
		if (op.cancelled) return false;
		if (!r) allOk = false;
	}
	if (op.move && allOk)
	{
		vector<Entry> rest;
		if (SUCCEEDED(p->dev.List(e.id, rest)) && rest.empty())
			p->dev.Delete(e.id, true);
	}
	return true;
}

/* ------------------------------------------------------------------------ */
/*  Копирование на устройство                                                */
/* ------------------------------------------------------------------------ */

typedef std::map<wstring, Entry> ChildMap; // ключ — имя в нижнем регистре

static void BuildChildMap(Panel *p, const wstring &parentId, ChildMap &m)
{
	m.clear();
	vector<Entry> kids;
	if (SUCCEEDED(p->dev.List(parentId, kids)))
		for (auto &k : kids) m[Lower(k.name)] = k;
}

static HRESULT CreateFileObject(Panel *p, const wstring &parentId, const wstring &name, unsigned __int64 size,
                                const GUID &contentType, IStream **stream, DWORD *opt)
{
	IPortableDeviceValues *v = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER,
	                              IID_IPortableDeviceValues, (void **)&v);
	if (FAILED(hr)) return hr;
	v->SetStringValue(WPD_OBJECT_PARENT_ID, parentId.c_str());
	v->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, name.c_str());
	v->SetStringValue(WPD_OBJECT_NAME, name.c_str());
	v->SetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, size);
	v->SetGuidValue(WPD_OBJECT_FORMAT, WPD_OBJECT_FORMAT_UNSPECIFIED);
	if (!IsEqualGUID(contentType, GUID_NULL)) v->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, contentType);
	hr = p->dev.content->CreateObjectWithPropertiesAndData(v, stream, opt, nullptr);
	v->Release();
	return hr;
}

static bool UploadFile(Panel *p, Op &op, const wstring &parentId, ChildMap &existing,
                       const wstring &localPath, const wstring &name)
{
	auto it = existing.find(Lower(name));
	if (it != existing.end())
	{
		if (it->second.dir)
			return op.ErrorContinue(L"На устройстве на месте файла уже есть папка", name, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
		if (!op.AskOverwrite(name, L"на устройстве")) return !op.cancelled;
		HRESULT hd = p->dev.Delete(it->second.id, false);
		if (FAILED(hd)) return op.ErrorContinue(L"Не удалось заменить файл на устройстве", name, hd);
		existing.erase(it);
	}

	HANDLE f = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return op.ErrorContinue(L"Не удалось открыть файл", localPath, HRESULT_FROM_WIN32(GetLastError()));
	LARGE_INTEGER li;
	li.QuadPart = 0;
	GetFileSizeEx(f, &li);
	unsigned __int64 size = (unsigned __int64)li.QuadPart;

	IStream *s = nullptr;
	DWORD opt = 0;
	HRESULT hr = CreateFileObject(p, parentId, name, size, WPD_CONTENT_TYPE_GENERIC_FILE, &s, &opt);
	if (FAILED(hr)) hr = CreateFileObject(p, parentId, name, size, WPD_CONTENT_TYPE_UNSPECIFIED, &s, &opt);
	if (FAILED(hr)) hr = CreateFileObject(p, parentId, name, size, GUID_NULL, &s, &opt);
	if (FAILED(hr))
	{
		CloseHandle(f);
		return op.ErrorContinue(L"Не удалось создать файл на устройстве", name, hr);
	}

	DWORD bufSize = opt ? opt : 256 * 1024;
	if (bufSize < 64 * 1024) bufSize = 64 * 1024;
	if (bufSize > 4 * 1024 * 1024) bufSize = 4 * 1024 * 1024;
	vector<BYTE> buf(bufSize);
	unsigned __int64 done = 0;
	bool ok = true;
	op.Progress(localPath, name, 0, size, true);
	for (;;)
	{
		DWORD got = 0;
		if (!ReadFile(f, buf.data(), bufSize, &got, nullptr))
		{
			hr = HRESULT_FROM_WIN32(GetLastError());
			ok = false;
			break;
		}
		if (got == 0) break;
		ULONG wr = 0;
		hr = s->Write(buf.data(), got, &wr);
		if (FAILED(hr)) { ok = false; break; }
		done += got;
		op.bytesDone += got;
		op.Progress(localPath, name, done, size);
		if (op.CheckCancel()) { ok = false; hr = S_OK; break; }
	}
	CloseHandle(f);

	if (ok)
	{
		hr = s->Commit(STGC_DEFAULT);
		if (FAILED(hr)) ok = false;
	}
	else
		s->Revert();

	if (ok)
	{
		Entry ne;
		ne.name = name;
		ne.size = size;
		IPortableDeviceDataStream *ds = nullptr;
		if (SUCCEEDED(s->QueryInterface(IID_IPortableDeviceDataStream, (void **)&ds)) && ds)
		{
			LPWSTR id = nullptr;
			if (SUCCEEDED(ds->GetObjectID(&id)) && id) { ne.id = id; CoTaskMemFree(id); }
			ds->Release();
		}
		if (!ne.id.empty()) existing[Lower(name)] = ne;
	}
	s->Release();

	if (!ok)
	{
		if (op.cancelled) return false;
		return op.ErrorContinue(L"Ошибка при копировании на устройство", name, hr);
	}
	op.filesDone++;
	if (op.move)
	{
		SetFileAttributesW(localPath.c_str(), FILE_ATTRIBUTE_NORMAL);
		if (!DeleteFileW(localPath.c_str()))
			return op.ErrorContinue(L"Файл скопирован, но не удалён с диска", localPath, HRESULT_FROM_WIN32(GetLastError()));
	}
	return true;
}

static bool UploadDir(Panel *p, Op &op, const wstring &parentId, ChildMap &existing,
                      const wstring &localPath, const wstring &name)
{
	wstring dirId;
	ChildMap kidsMap;
	auto it = existing.find(Lower(name));
	if (it != existing.end())
	{
		if (!it->second.dir)
			return op.ErrorContinue(L"На устройстве на месте папки уже есть файл", name, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
		dirId = it->second.id;
		BuildChildMap(p, dirId, kidsMap);
	}
	else
	{
		HRESULT hr = p->dev.CreateFolder(parentId, name, dirId);
		if (FAILED(hr) || dirId.empty())
			return op.ErrorContinue(L"Не удалось создать папку на устройстве", name, FAILED(hr) ? hr : E_FAIL);
		Entry ne;
		ne.id = dirId;
		ne.name = name;
		ne.dir = true;
		existing[Lower(name)] = ne;
	}

	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(JoinPath(localPath, L"*").c_str(), &fd);
	bool allOk = true;
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
			if (op.CheckCancel()) break;
			wstring child = JoinPath(localPath, fd.cFileName);
			bool r = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			         ? UploadDir(p, op, dirId, kidsMap, child, fd.cFileName)
			         : UploadFile(p, op, dirId, kidsMap, child, fd.cFileName);
			if (op.cancelled) break;
			if (!r) allOk = false;
		}
		while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	if (op.cancelled) return false;
	if (op.move && allOk)
	{
		SetFileAttributesW(localPath.c_str(), FILE_ATTRIBUTE_NORMAL);
		RemoveDirectoryW(localPath.c_str());
	}
	return true;
}

/* ------------------------------------------------------------------------ */
/*  Экспортируемые функции Far 2                                             */
/* ------------------------------------------------------------------------ */

extern "C"
{

int WINAPI GetMinFarVersionW(void)
{
	return MAKEFARVERSION(2, 0, 1000);
}

void WINAPI SetStartupInfoW(const struct PluginStartupInfo *psi)
{
	Info = *psi;
	FSF = *psi->FSF;
	Info.FSF = &FSF;
}

void WINAPI GetPluginInfoW(struct PluginInfo *pi)
{
	static const wchar_t *menu[1];
	menu[0] = L"MTP: телефоны и планшеты";
	pi->StructSize = sizeof(*pi);
	pi->Flags = 0;
	pi->DiskMenuStrings = menu;
	pi->Reserved0 = nullptr;
	pi->DiskMenuStringsNumber = 1;
	pi->PluginMenuStrings = menu;
	pi->PluginMenuStringsNumber = 1;
	pi->PluginConfigStrings = nullptr;
	pi->PluginConfigStringsNumber = 0;
	pi->CommandPrefix = L"mtp";
}

HANDLE WINAPI OpenPluginW(int OpenFrom, INT_PTR Item)
{
	Panel *p = new Panel();
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	p->comInit = (hr == S_OK || hr == S_FALSE);
	EnumDevices(p->devs);
	p->UpdateStrings();

	if (OpenFrom == OPEN_COMMANDLINE && Item)
	{
		wstring cmd = (const wchar_t *)Item;
		while (!cmd.empty() && cmd[0] == L' ') cmd.erase(0, 1);
		if (!cmd.empty() && !p->SetDir(cmd[0] == L'\\' ? cmd : L"\\" + cmd, false))
			ShowError(L"Путь не найден", cmd, S_OK);
	}
	if (p->devs.empty() && OpenFrom != OPEN_COMMANDLINE)
		ShowInfo(L"MTP-устройства не найдены. Подключите телефон, разблокируйте его и выберите режим USB «Передача файлов».");
	return (HANDLE)p;
}

void WINAPI ClosePluginW(HANDLE hPlugin)
{
	Panel *p = (Panel *)hPlugin;
	bool un = p->comInit;
	p->dev.Close();
	delete p;
	if (un) CoUninitialize();
}

void WINAPI GetOpenPluginInfoW(HANDLE hPlugin, struct OpenPluginInfo *oi)
{
	Panel *p = (Panel *)hPlugin;
	oi->StructSize = sizeof(*oi);
	oi->Flags = OPIF_USEFILTER | OPIF_USESORTGROUPS | OPIF_USEHIGHLIGHTING | OPIF_ADDDOTS | OPIF_SHOWPRESERVECASE;
	oi->HostFile = nullptr;
	oi->CurDir = p->curDir.c_str();
	oi->Format = PLUGIN_TITLE;
	oi->PanelTitle = p->title.c_str();
	oi->InfoLines = nullptr;
	oi->InfoLinesNumber = 0;
	oi->DescrFiles = nullptr;
	oi->DescrFilesNumber = 0;
	oi->PanelModesArray = nullptr;
	oi->PanelModesNumber = 0;
	oi->StartPanelMode = 0;
	oi->StartSortMode = 0;
	oi->StartSortOrder = 0;
	oi->KeyBar = nullptr;
	oi->ShortcutData = nullptr;
}

int WINAPI GetFindDataW(HANDLE hPlugin, struct PluginPanelItem **pItems, int *pNumber, int OpMode)
{
	Panel *p = (Panel *)hPlugin;
	*pItems = nullptr;
	*pNumber = 0;
	bool quiet = (OpMode & (OPM_SILENT | OPM_FIND)) != 0;

	vector<Entry> list;
	if (p->AtRoot())
	{
		EnumDevices(p->devs);
		for (auto &d : p->devs)
		{
			Entry e;
			e.name = d.name;
			e.dir = true;
			list.push_back(e);
		}
	}
	else
	{
		HANDLE scr = quiet ? nullptr : Info.SaveScreen(0, 0, -1, -1);
		if (!quiet)
		{
			vector<wstring> l = { PLUGIN_TITLE, L"Чтение содержимого папки...", Truncate(p->curDir, 60) };
			FarMsg(0, l, 0);
		}
		HRESULT hr = p->ListCurrent(list);
		if (scr) Info.RestoreScreen(scr);
		if (FAILED(hr))
		{
			if (!quiet) ShowError(L"Не удалось прочитать папку", p->curDir, hr);
			return FALSE;
		}
	}

	if (list.empty()) return TRUE;
	PluginPanelItem *items = new PluginPanelItem[list.size()];
	memset(items, 0, sizeof(PluginPanelItem) * list.size());
	for (size_t i = 0; i < list.size(); i++)
	{
		const Entry &e = list[i];
		PluginPanelItem &it = items[i];
		it.FindData.dwFileAttributes = e.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
		it.FindData.ftLastWriteTime = e.mtime;
		it.FindData.ftCreationTime = e.mtime;
		it.FindData.ftLastAccessTime = e.mtime;
		it.FindData.nFileSize = e.size;
		it.FindData.nPackSize = e.size;
		it.FindData.lpwszFileName = DupStr(e.name);
		it.FindData.lpwszAlternateFileName = nullptr;
		it.UserData = e.id.empty() ? 0 : (DWORD_PTR)DupStr(e.id);
	}
	*pItems = items;
	*pNumber = (int)list.size();
	return TRUE;
}

void WINAPI FreeFindDataW(HANDLE, struct PluginPanelItem *items, int n)
{
	if (!items) return;
	for (int i = 0; i < n; i++)
	{
		delete[] const_cast<wchar_t *>(items[i].FindData.lpwszFileName);
		delete[] (wchar_t *)items[i].UserData;
	}
	delete[] items;
}

int WINAPI SetDirectoryW(HANDLE hPlugin, const wchar_t *Dir, int OpMode)
{
	Panel *p = (Panel *)hPlugin;
	if (!Dir) return FALSE;
	return p->SetDir(Dir, (OpMode & (OPM_SILENT | OPM_FIND)) != 0) ? TRUE : FALSE;
}

static bool ItemToEntry(Panel *p, const PluginPanelItem &it, Entry &e)
{
	e.name = it.FindData.lpwszFileName ? it.FindData.lpwszFileName : L"";
	e.dir = (it.FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	e.size = it.FindData.nFileSize;
	e.mtime = it.FindData.ftLastWriteTime;
	if (it.UserData) { e.id = (const wchar_t *)it.UserData; return true; }
	Entry found;
	if (p->FindChild(e.name, found)) { e.id = found.id; return true; }
	return false;
}

int WINAPI GetFilesW(HANDLE hPlugin, struct PluginPanelItem *PanelItem, int ItemsNumber, int Move,
                     const wchar_t **DestPath, int OpMode)
{
	Panel *p = (Panel *)hPlugin;
	if (p->AtRoot() || ItemsNumber <= 0) return 0;

	static wstring dest;
	dest = (DestPath && *DestPath) ? *DestPath : L"";
	bool silent = (OpMode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) != 0;

	if (!silent)
	{
		wchar_t sub[128];
		if (ItemsNumber == 1)
			wsprintfW(sub, Move ? L"Переместить \"%.60s\" в:" : L"Копировать \"%.60s\" в:", PanelItem[0].FindData.lpwszFileName);
		else
			wsprintfW(sub, Move ? L"Переместить %d объект(ов) в:" : L"Копировать %d объект(ов) в:", ItemsNumber);
		wchar_t buf[MAX_PATH * 4];
		if (!Info.InputBox(Move ? L"Перемещение" : L"Копирование", sub, L"Copy", dest.c_str(), buf,
		                   (int)(sizeof(buf) / sizeof(buf[0])), nullptr, FIB_BUTTONS | FIB_EDITPATH | FIB_EXPANDENV))
			return -1;
		dest = buf;
		while (dest.size() > 3 && (dest.back() == L'\\' || dest.back() == L'/')) dest.pop_back();
		if (DestPath) *DestPath = dest.c_str();
	}
	if (dest.empty()) return 0;
	if (!ForceDirectories(dest))
	{
		ShowError(L"Не удалось создать папку назначения", dest, HRESULT_FROM_WIN32(GetLastError()));
		return 0;
	}

	Op op;
	op.silent = silent;
	op.move = Move != 0;
	op.title = Move ? L"Перемещение с устройства" : L"Копирование с устройства";
	op.Begin();
	int okCount = 0;
	for (int i = 0; i < ItemsNumber && !op.cancelled; i++)
	{
		Entry e;
		if (!ItemToEntry(p, PanelItem[i], e))
		{
			if (!op.ErrorContinue(L"Объект не найден на устройстве", e.name, S_OK)) break;
			continue;
		}
		bool r = e.dir ? DownloadDir(p, op, e, dest) : DownloadFile(p, op, e, dest);
		if (r && !op.cancelled)
		{
			PanelItem[i].Flags &= ~PPIF_SELECTED;
			okCount++;
		}
	}
	op.End();
	if (op.move) { p->cache.clear(); p->cacheParent.clear(); }
	if (op.cancelled) return okCount ? 1 : -1;
	return okCount ? 1 : 0;
}

int WINAPI PutFilesW(HANDLE hPlugin, struct PluginPanelItem *PanelItem, int ItemsNumber, int Move,
                     const wchar_t *SrcPath, int OpMode)
{
	Panel *p = (Panel *)hPlugin;
	bool silent = (OpMode & (OPM_SILENT | OPM_FIND | OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) != 0;
	if (p->AtRoot() || p->path.empty())
	{
		if (!silent) ShowInfo(p->AtRoot() ? L"Сначала откройте устройство и хранилище (например, «Внутренний общий накопитель»)."
		                                  : L"Сначала откройте хранилище (например, «Внутренний общий накопитель»).");
		return 0;
	}
	if (p->dev.readOnly)
	{
		if (!silent) ShowInfo(L"Устройство открыто только для чтения.");
		return 0;
	}
	if (ItemsNumber <= 0) return 0;

	if (!silent)
	{
		wchar_t sub[160];
		if (ItemsNumber == 1)
			wsprintfW(sub, Move ? L"Переместить \"%.60s\" на устройство?" : L"Копировать \"%.60s\" на устройство?", PanelItem[0].FindData.lpwszFileName);
		else
			wsprintfW(sub, Move ? L"Переместить %d объект(ов) на устройство?" : L"Копировать %d объект(ов) на устройство?", ItemsNumber);
		vector<wstring> l = { Move ? L"Перемещение" : L"Копирование", sub, Truncate(L"\x2192 " + p->curDir, 60),
		                      Move ? L"Переместить" : L"Копировать", L"Отмена" };
		if (FarMsg(0, l, 2) != 0) return -1;
	}

	wstring src = SrcPath ? SrcPath : L"";
	if (src.empty())
	{
		wchar_t cd[MAX_PATH * 2];
		DWORD n = GetCurrentDirectoryW(MAX_PATH * 2, cd);
		if (n) src = cd;
	}

	Op op;
	op.silent = silent;
	op.move = Move != 0;
	op.title = Move ? L"Перемещение на устройство" : L"Копирование на устройство";
	if (OpMode & OPM_EDIT) op.policy = OW_ALL; // после редактирования F4 файл просто заменяется
	op.Begin();

	wstring parentId = p->CurId();
	ChildMap existing;
	BuildChildMap(p, parentId, existing);

	int okCount = 0;
	for (int i = 0; i < ItemsNumber && !op.cancelled; i++)
	{
		const PluginPanelItem &it = PanelItem[i];
		wstring nm = it.FindData.lpwszFileName ? it.FindData.lpwszFileName : L"";
		if (nm.empty() || nm == L"..") continue;
		wstring full = IsAbsLocalPath(nm) ? nm : JoinPath(src, nm);
		size_t sl = nm.find_last_of(L"\\/");
		wstring shortName = sl == wstring::npos ? nm : nm.substr(sl + 1);
		bool r = (it.FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		         ? UploadDir(p, op, parentId, existing, full, shortName)
		         : UploadFile(p, op, parentId, existing, full, shortName);
		if (r && !op.cancelled)
		{
			PanelItem[i].Flags &= ~PPIF_SELECTED;
			okCount++;
		}
	}
	op.End();
	p->cache.clear();
	p->cacheParent.clear();
	if (op.cancelled) return okCount ? 1 : -1;
	return okCount ? 1 : 0;
}

int WINAPI DeleteFilesW(HANDLE hPlugin, struct PluginPanelItem *PanelItem, int ItemsNumber, int OpMode)
{
	Panel *p = (Panel *)hPlugin;
	if (p->AtRoot() || p->path.empty() || ItemsNumber <= 0)
	{
		if (!(OpMode & OPM_SILENT)) ShowInfo(L"Здесь удалять нельзя: это устройство или хранилище.");
		return FALSE;
	}
	if (!(OpMode & OPM_SILENT))
	{
		wchar_t sub[160];
		if (ItemsNumber == 1)
			wsprintfW(sub, L"Удалить \"%.60s\" с устройства?", PanelItem[0].FindData.lpwszFileName);
		else
			wsprintfW(sub, L"Удалить %d объект(ов) с устройства?", ItemsNumber);
		vector<wstring> l = { L"Удаление", sub, L"Корзины на телефоне нет — удаление безвозвратное.",
		                      L"Удалить", L"Отмена" };
		if (FarMsg(FMSG_WARNING, l, 2) != 0) return FALSE;
	}

	HANDLE scr = Info.SaveScreen(0, 0, -1, -1);
	bool any = false;
	for (int i = 0; i < ItemsNumber; i++)
	{
		Entry e;
		if (!ItemToEntry(p, PanelItem[i], e)) continue;
		vector<wstring> l = { L"Удаление", Truncate(e.name, 60) };
		FarMsg(0, l, 0);
		HRESULT hr = p->dev.Delete(e.id, e.dir);
		if (FAILED(hr))
		{
			vector<wstring> el = { L"Ошибка", L"Не удалось удалить", Truncate(e.name, 60), HrText(hr),
			                       L"Пропустить", L"Отмена" };
			if (FarMsg(FMSG_WARNING, el, 2) != 0) break;
		}
		else any = true;
		if (EscPressed() && ConfirmAbort()) break;
	}
	Info.RestoreScreen(scr);
	p->cache.clear();
	p->cacheParent.clear();
	return any ? TRUE : FALSE;
}

int WINAPI MakeDirectoryW(HANDLE hPlugin, const wchar_t **Name, int OpMode)
{
	Panel *p = (Panel *)hPlugin;
	if (p->AtRoot() || p->path.empty())
	{
		if (!(OpMode & OPM_SILENT)) ShowInfo(L"Папку можно создать только внутри хранилища.");
		return 0;
	}
	static wstring newName;
	newName = (Name && *Name) ? *Name : L"";
	if (!(OpMode & OPM_SILENT))
	{
		wchar_t buf[512];
		if (!Info.InputBox(L"Создание папки", L"Создать папку", L"NewFolder", newName.c_str(), buf,
		                   (int)(sizeof(buf) / sizeof(buf[0])), nullptr, FIB_BUTTONS))
			return -1;
		newName = buf;
	}
	while (!newName.empty() && newName.back() == L' ') newName.pop_back();
	while (!newName.empty() && newName[0] == L' ') newName.erase(0, 1);
	if (newName.empty()) return -1;
	if (newName.find_first_of(L"\\/") != wstring::npos)
	{
		ShowError(L"Имя папки не должно содержать \\ и /", newName, S_OK);
		return 0;
	}
	Entry ex;
	if (p->FindChild(newName, ex, true))
	{
		if (!(OpMode & OPM_SILENT)) ShowError(L"Такой объект уже существует", newName, S_OK);
		return 0;
	}
	wstring id;
	HRESULT hr = p->dev.CreateFolder(p->CurId(), newName, id);
	p->cache.clear();
	p->cacheParent.clear();
	if (FAILED(hr))
	{
		if (!(OpMode & OPM_SILENT)) ShowError(L"Не удалось создать папку", newName, hr);
		return 0;
	}
	if (Name) *Name = newName.c_str();
	return 1;
}

int WINAPI ProcessKeyW(HANDLE hPlugin, int Key, unsigned int ControlState)
{
	Panel *p = (Panel *)hPlugin;
	if (ControlState & PKF_PREPROCESS) return FALSE;

	// Shift+F6 — переименование
	if (Key == VK_F6 && ControlState == PKF_SHIFT)
	{
		if (p->AtRoot() || p->path.empty()) return TRUE;
		int size = Info.Control(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, 0);
		if (size <= 0) return TRUE;
		vector<char> buf(size);
		PluginPanelItem *it = (PluginPanelItem *)buf.data();
		Info.Control(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, (LONG_PTR)it);
		wstring oldName = it->FindData.lpwszFileName ? it->FindData.lpwszFileName : L"";
		if (oldName.empty() || oldName == L"..") return TRUE;
		Entry e;
		if (!ItemToEntry(p, *it, e)) return TRUE;

		wchar_t nb[512];
		if (!Info.InputBox(L"Переименование", L"Новое имя:", L"MtpRename", oldName.c_str(), nb,
		                   (int)(sizeof(nb) / sizeof(nb[0])), nullptr, FIB_BUTTONS))
			return TRUE;
		wstring nn = nb;
		if (nn.empty() || nn == oldName) return TRUE;
		if (nn.find_first_of(L"\\/") != wstring::npos)
		{
			ShowError(L"Имя не должно содержать \\ и /", nn, S_OK);
			return TRUE;
		}
		HRESULT hr = p->dev.Rename(e.id, nn);
		if (FAILED(hr)) ShowError(L"Не удалось переименовать", oldName, hr);
		p->cache.clear();
		p->cacheParent.clear();
		Info.Control(PANEL_ACTIVE, FCTL_UPDATEPANEL, 1, 0);
		Info.Control(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, 0);
		return TRUE;
	}
	return FALSE;
}

void WINAPI ExitFARW(void)
{
}

} // extern "C"
