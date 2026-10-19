#include "common.h"
#include "tinyxml2.h"
#include <shlwapi.h>
#include <commdlg.h>
namespace ttp::clienc {
static std::wstring directory(HMODULE h){std::wstring p(32768,L'\0');auto n=GetModuleFileNameW(h,p.data(),DWORD(p.size()));require(n&&n<p.size());p.resize(n);p.resize(p.find_last_of(L"\\/")+1);return p;}
std::wstring module_directory(){return directory(module);}
std::wstring host_directory(){return directory(nullptr);}
static std::wstring config_path(){return module_directory()+L"ttp_clienc.xml";}
Settings settings(){
    Settings s; auto path=config_path();
    ComPtr<IStream> file;auto hr=SHCreateStreamOnFileEx(path.c_str(),STGM_READ|STGM_SHARE_DENY_WRITE,0,FALSE,nullptr,file.put());
    if(hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)||hr==STG_E_FILENOTFOUND){
        s.presets.push_back({L"FFmpeg WAV",L"ffmpeg.exe",L"wav",L"-nostdin -y -f wav -ignore_length 1 -i pipe:0 -c:a pcm_s16le \"%d\"",16,0});return s;
    }
    check(hr);auto n=stream_size(file.p);require(n>0&&n<=2*1024*1024);auto bytes=read_at(file.p,0,size_t(n));
    tinyxml2::XMLDocument doc;require(doc.Parse(reinterpret_cast<const char *>(bytes.data()),bytes.size())==tinyxml2::XML_SUCCESS);
    auto root=doc.FirstChildElement("ttp_cmdline_encoder");require(root);s.current=root->UnsignedAttribute("current",0);
    for(auto e=root->FirstChildElement("preset");e;e=e->NextSiblingElement("preset")){
        require(s.presets.size()<256);Preset p;auto val=[&](const char*k){const char*v=e->Attribute(k);return wide(v?v:"");};
        p.name=val("name");p.encoder=val("encoder");p.ext=val("ext");p.param=val("param");p.bits=e->UnsignedAttribute("bps",16);p.tag_type=e->UnsignedAttribute("tagtype",0);
        require(!p.name.empty()&&!p.encoder.empty()&&!p.ext.empty());require(p.bits==0||p.bits==8||p.bits==16||p.bits==24||p.bits==32);s.presets.push_back(std::move(p));
    }
    require(!s.presets.empty());if(s.current>=s.presets.size())s.current=0;return s;
}
void save_settings(const Settings&s){
    require(!s.presets.empty()&&s.presets.size()<=256&&s.current<s.presets.size());tinyxml2::XMLDocument d;d.InsertEndChild(d.NewDeclaration());auto root=d.NewElement("ttp_cmdline_encoder");root->SetAttribute("version",1);root->SetAttribute("current",unsigned(s.current));d.InsertEndChild(root);
    for(auto&p:s.presets){require(!p.name.empty()&&!p.encoder.empty()&&!p.ext.empty());auto e=d.NewElement("preset");e->SetAttribute("name",utf8(p.name).c_str());e->SetAttribute("encoder",utf8(p.encoder).c_str());e->SetAttribute("ext",utf8(p.ext).c_str());e->SetAttribute("param",utf8(p.param).c_str());e->SetAttribute("bps",p.bits);e->SetAttribute("tagtype",p.tag_type);root->InsertEndChild(e);}
    tinyxml2::XMLPrinter text;d.Print(&text);auto path=config_path();wchar_t temp[MAX_PATH]{};require(GetTempFileNameW(module_directory().c_str(),L"cli",0,temp)!=0,HRESULT_FROM_WIN32(GetLastError()));
    auto hr=protect([&]{ComPtr<IStream> f;check(SHCreateStreamOnFileEx(temp,STGM_CREATE|STGM_READWRITE|STGM_SHARE_EXCLUSIVE,0,TRUE,nullptr,f.put()));write_exact(f.p,text.CStr(),DWORD(text.CStrSize()-1));commit_stream(f.p);f.reset();require(MoveFileExW(temp,path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,HRESULT_FROM_WIN32(GetLastError()));return S_OK;});if(FAILED(hr))DeleteFileW(temp);check(hr);
}
static std::wstring control(HWND h,int id){auto c=GetDlgItem(h,id);int n=GetWindowTextLengthW(c);require(n>=0&&n<32768);std::wstring v(size_t(n)+1,L'\0');GetWindowTextW(c,v.data(),n+1);v.resize(n);return v;}
static void show(HWND h,const Preset&p){SetDlgItemTextW(h,101,p.name.c_str());SetDlgItemTextW(h,102,p.encoder.c_str());SetDlgItemTextW(h,103,p.ext.c_str());SetDlgItemTextW(h,104,p.param.c_str());SetDlgItemInt(h,105,p.bits,FALSE);SetDlgItemInt(h,106,p.tag_type,FALSE);}
static Preset collect(HWND h){Preset p;p.name=control(h,101);p.encoder=control(h,102);p.ext=control(h,103);p.param=control(h,104);p.bits=GetDlgItemInt(h,105,nullptr,FALSE);p.tag_type=GetDlgItemInt(h,106,nullptr,FALSE);require(!p.name.empty()&&!p.encoder.empty()&&!p.ext.empty(),E_INVALIDARG);require(p.ext.find_first_of(L"\\/:*?\"<>|")==std::wstring::npos,E_INVALIDARG);require(p.bits==0||p.bits==8||p.bits==16||p.bits==24||p.bits==32,E_INVALIDARG);return p;}
static void refresh(HWND h,Settings&s){SendDlgItemMessageW(h,100,CB_RESETCONTENT,0,0);for(auto&p:s.presets)SendDlgItemMessageW(h,100,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(p.name.c_str()));SendDlgItemMessageW(h,100,CB_SETCURSEL,s.current,0);show(h,s.presets[s.current]);}
static INT_PTR CALLBACK dialog(HWND h,UINT msg,WPARAM w,LPARAM l){
    auto s=reinterpret_cast<Settings *>(GetWindowLongPtrW(h,DWLP_USER));
    auto hr=protect([&]()->HRESULT{
        if(msg==WM_INITDIALOG){s=reinterpret_cast<Settings*>(l);SetWindowLongPtrW(h,DWLP_USER,l);refresh(h,*s);return S_OK;}
        if(msg==WM_CLOSE||(msg==WM_COMMAND&&LOWORD(w)==IDCANCEL)){EndDialog(h,IDCANCEL);return S_OK;}
        if(msg!=WM_COMMAND||!s)return S_FALSE;
        auto id=LOWORD(w);
        if(id==100&&HIWORD(w)==CBN_SELCHANGE){auto next=SendDlgItemMessageW(h,100,CB_GETCURSEL,0,0);require(next>=0&&size_t(next)<s->presets.size());s->presets[s->current]=collect(h);s->current=size_t(next);show(h,s->presets[s->current]);return S_OK;}
        if(id==107){wchar_t file[32768]{};OPENFILENAMEW o{sizeof(o)};o.hwndOwner=h;o.lpstrFilter=L"编码器程序\0*.exe\0所有文件\0*.*\0";o.lpstrFile=file;o.nMaxFile=32768;o.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;if(GetOpenFileNameW(&o))SetDlgItemTextW(h,102,file);return S_OK;}
        if(id==108){s->presets[s->current]=collect(h);require(s->presets.size()<256);s->presets.push_back(s->presets[s->current]);s->current=s->presets.size()-1;s->presets.back().name+=L"（副本）";refresh(h,*s);return S_OK;}
        if(id==109){require(s->presets.size()>1,E_INVALIDARG);s->presets.erase(s->presets.begin()+s->current);s->current=std::min(s->current,s->presets.size()-1);refresh(h,*s);return S_OK;}
        if(id==IDOK){s->presets[s->current]=collect(h);save_settings(*s);EndDialog(h,IDOK);return S_OK;}return S_FALSE;
    });if(FAILED(hr))MessageBoxW(h,L"设置无效或无法保存。请检查编码器、扩展名、位深及目录写入权限。",L"命令行编码器",MB_ICONERROR|MB_OK);return hr==S_FALSE?FALSE:TRUE;
}
HRESULT configure(HWND owner){return protect([&]{auto s=settings();auto n=DialogBoxParamW(module,MAKEINTRESOURCEW(100),owner,dialog,reinterpret_cast<LPARAM>(&s));return n==-1?HRESULT_FROM_WIN32(GetLastError()):n==IDOK?S_OK:S_FALSE;});}
}

