#include "common.h"
#include <shlwapi.h>
#include <cmath>
#include <ks.h>
#include <ksmedia.h>

namespace ttp::clienc {
namespace {
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void reset(HANDLE h = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = h;
    }
};
void wincheck(BOOL ok) { if (!ok) throw Failure{HRESULT_FROM_WIN32(GetLastError())}; }
void write_handle(HANDLE handle, const BYTE *data, size_t size) {
    while (size) {
        DWORD written = 0, count = DWORD(std::min<size_t>(size, 65536));
        wincheck(WriteFile(handle, data, count, &written, nullptr));
        require(written && written <= count, STG_E_WRITEFAULT);
        data += written; size -= written;
    }
}
void replace_all(std::wstring &s, const wchar_t *key, const std::wstring &value) {
    size_t at = 0, length = wcslen(key);
    while ((at = s.find(key, at)) != std::wstring::npos) {
        s.replace(at, length, value); at += value.size();
    }
}
void little16(Bytes &b, WORD x) { b.push_back(BYTE(x)); b.push_back(BYTE(x >> 8)); }
void little32(Bytes &b, DWORD x) { little16(b, WORD(x)); little16(b, WORD(x >> 16)); }
Bytes wave_header(const WAVEFORMATEX &f, DWORD bytes) {
    Bytes b;
    little32(b, 0x46464952); little32(b, bytes == MAXDWORD ? MAXDWORD : bytes + 36);
    little32(b, 0x45564157); little32(b, 0x20746d66); little32(b, 16);
    little16(b, f.wFormatTag); little16(b, f.nChannels); little32(b, f.nSamplesPerSec);
    little32(b, f.nAvgBytesPerSec); little16(b, f.nBlockAlign); little16(b, f.wBitsPerSample);
    little32(b, 0x61746164); little32(b, bytes); return b;
}
}

class CommandEncoder final : public Encoder, public Metadata {
    LONG references_{1};
    enum class State { empty, ready, running, finished, failed } state_{State::empty};
    HRESULT result_{S_OK};
    WAVEFORMATEX input_{}, output_{};
    bool floating_{}, file_input_{};
    Preset preset_;
    std::wstring target_, reservation_, source_, encoded_, log_;
    std::vector<std::pair<std::string, std::wstring>> tags_;
    uint64_t bytes_{};
    Handle source_file_, process_, job_, pipe_;

    void terminate() noexcept {
        // A child may exit while its descendants still hold inherited handles.
        // Close the entire job even when the immediate child has already exited.
        if (job_.value) TerminateJobObject(job_.value, ERROR_CANCELLED);
        if (process_.value && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT) {
            if (!job_.value) TerminateProcess(process_.value, ERROR_CANCELLED);
            WaitForSingleObject(process_.value, 5000);
        }
    }
    void close_input() noexcept { pipe_.reset(); }
    void clean() noexcept {
        terminate(); close_input(); source_file_.reset();
        for (const auto *p : {&source_, &encoded_, &log_, &reservation_})
            if (!p->empty()) DeleteFileW(p->c_str());
    }
    void write_audio(const BYTE *data, size_t size) {
        if (file_input_) { write_handle(source_file_.value, data, size); return; }
        struct Operation {
            OVERLAPPED overlapped{};
            Handle event;
            Bytes bytes;
            Operation(const BYTE *p, size_t n) : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)), bytes(p, p + n) {
                require(event.value, E_OUTOFMEMORY); overlapped.hEvent = event.value;
            }
        };
        while (size) {
            DWORD count = DWORD(std::min<size_t>(size, 65536)), written{};
            auto operation = std::make_unique<Operation>(data, count);
            if (!WriteFile(pipe_.value, operation->bytes.data(), count, &written, &operation->overlapped)) {
                DWORD error = GetLastError();
                require(error == ERROR_IO_PENDING, HRESULT_FROM_WIN32(error));
                HANDLE waits[]{operation->event.value, process_.value};
                DWORD status = WaitForMultipleObjects(2, waits, FALSE, 30000);
                if (status != WAIT_OBJECT_0) {
                    // CancelIo is available on XP and cancels this calling
                    // thread's write. It does not depend on child/job lifetime.
                    CancelIo(pipe_.value);
                    bool completed = WaitForSingleObject(operation->event.value, 5000) == WAIT_OBJECT_0;
                    if (!completed) {
                        pipe_.reset();
                        completed = WaitForSingleObject(operation->event.value, 5000) == WAIT_OBJECT_0;
                    }
                    // Even a faulty driver must not make a cancelled operation
                    // reference freed memory. This exceptional orphan owns only
                    // its buffer/event/OVERLAPPED, never the encoder or DLL code.
                    if (!completed) operation.release();
                    terminate(); close_input();
                    throw Failure{status == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : STG_E_WRITEFAULT};
                }
                wincheck(GetOverlappedResult(pipe_.value, &operation->overlapped, &written, FALSE));
            }
            require(written && written <= count, STG_E_WRITEFAULT);
            data += written; size -= written;
        }
    }
    void configure_format(const WAVEFORMATEX *p) {
        require(p, E_POINTER); input_ = *p;
        WORD tag = p->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            require(p->cbSize >= 22, E_INVALIDARG);
            auto ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(p);
            if (same(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
            else if (same(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
            else throw Failure{E_INVALIDARG};
            require(!ext->Samples.wValidBitsPerSample || ext->Samples.wValidBitsPerSample <= p->wBitsPerSample, E_INVALIDARG);
        }
        require(tag == WAVE_FORMAT_PCM || tag == WAVE_FORMAT_IEEE_FLOAT, E_INVALIDARG);
        floating_ = tag == WAVE_FORMAT_IEEE_FLOAT;
        require(p->nChannels && p->nChannels <= 32 && p->nSamplesPerSec && p->nSamplesPerSec <= 1536000, E_INVALIDARG);
        require(floating_ ? (p->wBitsPerSample == 32 || p->wBitsPerSample == 64) :
            (p->wBitsPerSample == 8 || p->wBitsPerSample == 16 || p->wBitsPerSample == 24 || p->wBitsPerSample == 32), E_INVALIDARG);
        require(p->nBlockAlign == p->nChannels * p->wBitsPerSample / 8, E_INVALIDARG);
        auto s = settings(); preset_ = s.presets[s.current];
        // Original runtime captures distinguish bps=32 (integer PCM) from the
        // source precision preset (bps=0). Do not infer the stored value from UI text.
        output_ = input_; output_.cbSize = 0;
        output_.wBitsPerSample = WORD(preset_.bits ? preset_.bits : input_.wBitsPerSample);
        output_.wFormatTag = preset_.bits == 0 ? tag : WAVE_FORMAT_PCM;
        output_.nBlockAlign = output_.nChannels * output_.wBitsPerSample / 8;
        output_.nAvgBytesPerSec = output_.nSamplesPerSec * output_.nBlockAlign;
        file_input_ = preset_.param.find(L"%s") != std::wstring::npos;
        require(preset_.param.find(L"%d") != std::wstring::npos, E_INVALIDARG);
        require(preset_.ext.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos, E_INVALIDARG);
    }
    double sample(const BYTE *p) const {
        if (floating_) {
            if (input_.wBitsPerSample == 64) { double v; memcpy(&v,p,8); return v; }
            float v; memcpy(&v,p,4); return v;
        }
        if (input_.wBitsPerSample == 8) return (int(*p)-128)/128.0;
        if (input_.wBitsPerSample == 16) { int16_t v; memcpy(&v,p,2); return v/32768.0; }
        if (input_.wBitsPerSample == 24) return int32_t(uint32_t(p[0])<<8 | uint32_t(p[1])<<16 | uint32_t(p[2])<<24)/2147483648.0;
        int32_t v; memcpy(&v,p,4); return v/2147483648.0;
    }
    void convert(const BYTE *data, size_t count, Bytes &out) const {
        size_t width = output_.wBitsPerSample/8;
        out.resize(count*width);
        for (size_t i=0;i<count;++i) {
            double v = sample(data+i*(input_.wBitsPerSample/8));
            if (output_.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                if(output_.wBitsPerSample==64)memcpy(out.data()+i*8,&v,8);
                else{float f = static_cast<float>(v); memcpy(out.data()+i*4,&f,4);}continue;
            }
            if (std::isnan(v)) v=0;
            v=std::max(-1.0,std::min(v,1.0));
            double scale=std::ldexp(1.0,output_.wBitsPerSample-1);
            double scaled=std::max(-scale,std::min(scale-1,v*scale));
            double lower=std::floor(scaled), fraction=scaled-lower;
            int64_t n=static_cast<int64_t>(lower);
            if(fraction>0.5||(fraction==0.5&&(n&1)))++n;
            if(width==1)out[i]=BYTE(n+128);
            else for(size_t k=0;k<width;++k)out[i*width+k]=BYTE(uint64_t(n)>>(8*k));
        }
    }
    void launch() {
        std::wstring executable=preset_.encoder;
        if(executable.size()>1&&executable.front()==L'"'&&executable.back()==L'"') executable=executable.substr(1,executable.size()-2);
        if(PathIsRelativeW(executable.c_str())&&executable.find_first_of(L"\\/")!=std::wstring::npos)
            executable=host_directory()+executable;
        std::wstring args=preset_.param;
        replace_all(args,L"%s",source_); replace_all(args,L"%d",encoded_);
        std::wstring command=L"\""+executable+L"\" "+args;
        require(command.size()<32767,E_INVALIDARG);
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        Handle input, output;
        if(file_input_) input.reset(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
        else {
            GUID id{};check(CoCreateGuid(&id));wchar_t suffix[40]{};
            require(StringFromGUID2(id,suffix,40),E_FAIL);
            std::wstring name=L"\\\\.\\pipe\\TTPlayerClienc-"+std::wstring(suffix);
            pipe_.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_OUTBOUND|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE|PIPE_WAIT,1,65536,65536,0,nullptr));
            require(pipe_.value!=INVALID_HANDLE_VALUE,HRESULT_FROM_WIN32(GetLastError()));
            input.reset(CreateFileW(name.c_str(),GENERIC_READ,0,&sa,OPEN_EXISTING,0,nullptr));
            require(input.value!=INVALID_HANDLE_VALUE,HRESULT_FROM_WIN32(GetLastError()));
            // The local client has connected synchronously before this call;
            // ERROR_PIPE_CONNECTED is the documented successful race result.
            OVERLAPPED connected{};Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
            require(event.value,E_OUTOFMEMORY);connected.hEvent=event.value;
            BOOL ok=ConnectNamedPipe(pipe_.value,&connected);
            require(ok||GetLastError()==ERROR_PIPE_CONNECTED,HRESULT_FROM_WIN32(GetLastError()));
        }
        require(input.value!=INVALID_HANDLE_VALUE,HRESULT_FROM_WIN32(GetLastError()));
        output.reset(CreateFileW(log_.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr));
        require(output.value!=INVALID_HANDLE_VALUE,HRESULT_FROM_WIN32(GetLastError()));
        STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;
        si.hStdInput=input.value;si.hStdOutput=si.hStdError=output.value;
        PROCESS_INFORMATION pi{};
        wincheck(CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,host_directory().c_str(),&si,&pi));
        process_.reset(pi.hProcess);Handle thread(pi.hThread);
        job_.reset(CreateJobObjectW(nullptr,nullptr));
        if(job_.value) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if(!SetInformationJobObject(job_.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))||!AssignProcessToJobObject(job_.value,process_.value)) job_.reset();
        }
        require(ResumeThread(thread.value)!=MAXDWORD,HRESULT_FROM_WIN32(GetLastError()));
        input.reset();output.reset();
    }
    void apply_tags() {
        if(tags_.empty()||!preset_.tag_type) return;
        auto host=standard_content();require(host,E_NOINTERFACE);
        ComPtr<IStream> stream;check(SHCreateStreamOnFileEx(encoded_.c_str(),STGM_READWRITE|STGM_SHARE_DENY_WRITE,0,FALSE,nullptr,stream.put()));
        ComPtr<Metadata> metadata;ULONGLONG bytes{};check(host(stream.p,preset_.tag_type,metadata.put(),&bytes));require(!!metadata,E_NOINTERFACE);
        for(auto&t:tags_)check(metadata->Set(t.first.c_str(),t.second.empty()?nullptr:t.second.c_str()));
        commit_stream(stream.p);stream.reset();metadata.reset();
    }
public:
    ~CommandEncoder() { clean(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void**out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(same(iid,IID_IUnknown)||same(iid,iid_encoder)||same(iid,iid_stream_encoder))*out=static_cast<Encoder*>(this);
        else if(same(iid,iid_metadata))*out=static_cast<Metadata*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&references_);}
    ULONG STDMETHODCALLTYPE Release() override{auto n=InterlockedDecrement(&references_);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE OpenPath(const wchar_t*path,const WAVEFORMATEX*f) override {
        return protect([&]{require(path&&*path,E_POINTER);require(state_==State::empty,E_UNEXPECTED);configure_format(f);
            wchar_t absolute[32768]{};DWORD n=GetFullPathNameW(path,32768,absolute,nullptr);require(n&&n<32768,HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE));target_=absolute;
            auto directory=target_.substr(0,target_.find_last_of(L"\\/")+1);wchar_t reserved[MAX_PATH]{};
            wincheck(GetTempFileNameW(directory.c_str(),L"cli",0,reserved));reservation_=reserved;
            source_=reservation_+L".input.wav";encoded_=reservation_+L"."+preset_.ext;log_=reservation_+L".log";
            state_=State::ready;return S_OK;});
    }
    HRESULT STDMETHODCALLTYPE OpenStream(IStream*stream,const WAVEFORMATEX*f) override {
        return protect([&]{require(stream,E_POINTER);STATSTG stat{};check(stream->Stat(&stat,0));
            std::wstring path=stat.pwcsName?stat.pwcsName:L"";CoTaskMemFree(stat.pwcsName);
            require(!path.empty(),STG_E_INVALIDNAME);
            // External programs need a named file, as in the original slot 8.
            // Do not retain the host's write lock across CreateProcess.
            return OpenPath(path.c_str(),f);});
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        if(state_!=State::ready)return E_UNEXPECTED;
        result_=protect([&]{
            if(file_input_){source_file_.reset(CreateFileW(source_.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr));require(source_file_.value!=INVALID_HANDLE_VALUE,HRESULT_FROM_WIN32(GetLastError()));}
            else launch();
            auto header=wave_header(output_,file_input_?0:MAXDWORD);write_audio(header.data(),header.size());return S_OK;});
        state_=SUCCEEDED(result_)?State::running:State::failed;if(FAILED(result_))clean();return result_;
    }
    HRESULT STDMETHODCALLTYPE Write(Buffer*buffer,DWORD*reported) override {
        if(reported)*reported=0;if(state_!=State::running)return state_==State::failed?result_:E_UNEXPECTED;
        auto hr=protect([&]{require(buffer,E_POINTER);BYTE*data{};DWORD bytes{},capacity{};check(buffer->Data(&data,&bytes));check(buffer->Capacity(&capacity));
            require(bytes<=capacity&&(!bytes||data)&&bytes%input_.nBlockAlign==0,E_INVALIDARG);
            uint64_t converted=uint64_t(bytes/input_.nBlockAlign)*output_.nBlockAlign;
            require(bytes_+converted<=MAXDWORD-36,HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
            Bytes out;DWORD offset=0;while(offset<bytes){DWORD count=std::min<DWORD>(bytes-offset,16384*input_.nBlockAlign);
                if(input_.wBitsPerSample==output_.wBitsPerSample&&floating_==(output_.wFormatTag==WAVE_FORMAT_IEEE_FLOAT))write_audio(data+offset,count);
                else{convert(data+offset,count/(input_.wBitsPerSample/8),out);write_audio(out.data(),out.size());}
                offset+=count;if(reported)*reported=offset;
            }bytes_+=converted;return S_OK;});
        if(FAILED(hr)&&hr!=E_INVALIDARG&&hr!=E_POINTER){state_=State::failed;result_=hr;clean();}return hr;
    }
    HRESULT STDMETHODCALLTYPE Finish() override {
        if(state_==State::finished||state_==State::failed)return result_;if(state_!=State::running)return E_UNEXPECTED;
        state_=State::failed;
        result_=protect([&]{
            if(file_input_){auto header=wave_header(output_,DWORD(bytes_));LARGE_INTEGER zero{};wincheck(SetFilePointerEx(source_file_.value,zero,nullptr,FILE_BEGIN));write_handle(source_file_.value,header.data(),header.size());wincheck(FlushFileBuffers(source_file_.value));source_file_.reset();launch();}
            else close_input();
            DWORD wait=WaitForSingleObject(process_.value,300000);require(wait==WAIT_OBJECT_0,HRESULT_FROM_WIN32(wait==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_PROCESS_ABORTED));
            DWORD code{};wincheck(GetExitCodeProcess(process_.value,&code));require(code==0,HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED));
            require(GetFileAttributesW(encoded_.c_str())!=INVALID_FILE_ATTRIBUTES,STG_E_FILENOTFOUND);
            apply_tags();wincheck(MoveFileExW(encoded_.c_str(),target_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH));return S_OK;});
        if(SUCCEEDED(result_))state_=State::finished;clean();return result_;
    }
    HRESULT STDMETHODCALLTYPE FileExtension(wchar_t**p) override {return protect([&]{if(state_!=State::empty)return text(preset_.ext,p);auto s=settings();return text(s.presets[s.current].ext,p);});}
    HRESULT STDMETHODCALLTYPE Count(DWORD*p) override{if(!p)return E_POINTER;*p=DWORD(tags_.size());return S_OK;}
    HRESULT STDMETHODCALLTYPE At(DWORD i,wchar_t**k,wchar_t**v) override{return protect([&]{require(k&&v,E_POINTER);*k=*v=nullptr;require(i<tags_.size(),E_INVALIDARG);check(text(wide(tags_[i].first),k));auto hr=text(tags_[i].second,v);if(FAILED(hr)){CoTaskMemFree(*k);*k=nullptr;}return hr;});}
    HRESULT STDMETHODCALLTYPE Get(const char*k,wchar_t**v) override{if(!k||!v)return E_POINTER;*v=nullptr;for(auto&t:tags_)if(!_stricmp(k,t.first.c_str()))return text(t.second,v);return S_FALSE;}
    HRESULT STDMETHODCALLTYPE Set(const char*k,const wchar_t*v) override{return protect([&]{require(k&&*k,E_INVALIDARG);require(state_!=State::finished&&state_!=State::failed,E_UNEXPECTED);for(auto&t:tags_)if(!_stricmp(k,t.first.c_str())){t.second=v?v:L"";return S_OK;}tags_.emplace_back(k,v?v:L"");return S_OK;});}
};
HRESULT make_encoder(void**out){if(!out)return E_POINTER;*out=nullptr;return protect([&]{*out=static_cast<Encoder*>(new CommandEncoder);return S_OK;});}
}
