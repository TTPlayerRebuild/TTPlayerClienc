#include "common.h"
namespace ttp::clienc {
HMODULE module{};
StandardContent standard_content() {
    for (const auto name : {L"ttpctrl.dll", L"soundcore.dll", static_cast<const wchar_t *>(nullptr)})
        if (auto h = GetModuleHandleW(name))
            if (auto p = GetProcAddress(h, "CreateStdContent"))
                return reinterpret_cast<StandardContent>(p);
    return nullptr;
}
class Factory final : public EncoderCreator {
    LONG refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (!p) return E_POINTER; *p = nullptr;
        if (!same(id,IID_IUnknown) && !same(id,cat_encoder)) return E_NOINTERFACE;
        *p = static_cast<EncoderCreator *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { auto n=InterlockedDecrement(&refs_); if(!n)delete this; return n; }
    HRESULT STDMETHODCALLTYPE Create(void **p) override { return make_encoder(p); }
    HRESULT STDMETHODCALLTYPE Name(wchar_t **p) override { return text(L"命令行编码器",p); }
    HRESULT STDMETHODCALLTYPE Extension(wchar_t **p) override { return protect([&]{auto s=settings();return text(s.presets[s.current].ext,p);}); }
    HRESULT STDMETHODCALLTYPE ConfigAvailable() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Configure(HWND p) override { return configure(p); }
};
class Plugin final : public AddIn {
    LONG refs_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if(!p)return E_POINTER;*p=nullptr;
        if(!same(id,IID_IUnknown)&&!same(id,iid_addin))return E_NOINTERFACE;
        *p=static_cast<AddIn *>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release() override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Enum(DWORD n,GUID *cat,void **p) override {
        if(!p)return E_POINTER;*p=nullptr;if(!cat)return E_POINTER;if(n)return E_INVALIDARG;
        return protect([&]{*cat=cat_encoder;*p=static_cast<EncoderCreator *>(new Factory);return S_OK;});
    }
};
}
extern "C" HRESULT WINAPI ttpGetSoundAddIn(void **p) {
    if(!p)return E_POINTER;*p=nullptr;
    return ttp::clienc::protect([&]{*p=static_cast<ttp::clienc::AddIn *>(new ttp::clienc::Plugin);return S_OK;});
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){ttp::clienc::module=h;DisableThreadLibraryCalls(h);}return TRUE;
}
