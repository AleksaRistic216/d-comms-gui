#include <napi.h>
#include <map>
#include <string>
#include <mutex>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "proto.h"
#include "sync.h"
#include "dht_client.h"
}

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

struct ChatEntry {
    proto_chat chat;
    std::string name;
};

static std::map<int, ChatEntry*> g_chats;
static int g_next_id = 0;
static std::mutex g_chats_mutex;

static int  g_sync_port    = -1;
static bool g_sync_started = false;
static bool g_dht_started  = false;

// ---------------------------------------------------------------------------
// AsyncWorker: proto_initialize (5-second KDF)
// ---------------------------------------------------------------------------

class CreateChatWorker : public Napi::AsyncWorker {
public:
    explicit CreateChatWorker(Napi::Env env, Napi::Promise::Deferred d,
                               std::string chatName)
        : Napi::AsyncWorker(env), deferred_(d), name_(std::move(chatName))
    {
        entry_ = new ChatEntry();
        memset(&entry_->chat, 0, sizeof(proto_chat));
        entry_->name = name_;
    }

    void Execute() override {
        int r = proto_initialize(&entry_->chat, user_key_, secret_id_);
        if (r != 0) {
            SetError("proto_initialize failed");
        }
    }

    void OnOK() override {
        std::lock_guard<std::mutex> lock(g_chats_mutex);
        int id = g_next_id++;
        g_chats[id] = entry_;

        auto obj = Napi::Object::New(Env());
        obj.Set("chatId",   Napi::Number::New(Env(), id));
        obj.Set("userKey",  Napi::String::New(Env(), user_key_));
        obj.Set("secretId", Napi::String::New(Env(), secret_id_));
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error& e) override {
        delete entry_;
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    ChatEntry* entry_ = nullptr;
    std::string name_;
    char user_key_[ID_BYTES * 2 + 2]  = {};
    char secret_id_[ID_BYTES * 2 + 2] = {};
};

// ---------------------------------------------------------------------------
// AsyncWorker: proto_join (5-second KDF)
// ---------------------------------------------------------------------------

class JoinChatWorker : public Napi::AsyncWorker {
public:
    JoinChatWorker(Napi::Env env, Napi::Promise::Deferred d,
                   std::string chatName, std::string uk, std::string si)
        : Napi::AsyncWorker(env), deferred_(d),
          name_(std::move(chatName)), userKey_(std::move(uk)), secretId_(std::move(si))
    {
        entry_ = new ChatEntry();
        memset(&entry_->chat, 0, sizeof(proto_chat));
        entry_->name = name_;
    }

    void Execute() override {
        proto_join(&entry_->chat, userKey_.c_str(), secretId_.c_str());
    }

    void OnOK() override {
        std::lock_guard<std::mutex> lock(g_chats_mutex);
        int id = g_next_id++;
        g_chats[id] = entry_;

        auto obj = Napi::Object::New(Env());
        obj.Set("chatId",   Napi::Number::New(Env(), id));
        obj.Set("userKey",  Napi::String::New(Env(), entry_->chat.user_key));
        obj.Set("secretId", Napi::String::New(Env(), entry_->chat.secret_id));
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error& e) override {
        delete entry_;
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    ChatEntry* entry_ = nullptr;
    std::string name_;
    std::string userKey_;
    std::string secretId_;
};

// ---------------------------------------------------------------------------
// AsyncWorker: proto_load_chat (5-second KDF)
// ---------------------------------------------------------------------------

class LoadChatWorker : public Napi::AsyncWorker {
public:
    LoadChatWorker(Napi::Env env, Napi::Promise::Deferred d,
                   std::string name, std::string basedir)
        : Napi::AsyncWorker(env), deferred_(d),
          name_(std::move(name)), basedir_(std::move(basedir))
    {
        entry_ = new ChatEntry();
        memset(&entry_->chat, 0, sizeof(proto_chat));
        entry_->name = name_;
    }

    void Execute() override {
        int r = proto_load_chat(&entry_->chat, name_.c_str(), basedir_.c_str());
        if (r != 0) {
            SetError("Failed to load chat '" + name_ + "'");
        }
    }

    void OnOK() override {
        std::lock_guard<std::mutex> lock(g_chats_mutex);
        int id = g_next_id++;
        g_chats[id] = entry_;

        auto obj = Napi::Object::New(Env());
        obj.Set("chatId",      Napi::Number::New(Env(), id));
        obj.Set("userKey",     Napi::String::New(Env(), entry_->chat.user_key));
        obj.Set("secretId",    Napi::String::New(Env(), entry_->chat.secret_id));
        obj.Set("isInitiator", Napi::Boolean::New(Env(), entry_->chat.is_initiator != 0));
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error& e) override {
        delete entry_;
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    ChatEntry* entry_ = nullptr;
    std::string name_;
    std::string basedir_;
};

// ---------------------------------------------------------------------------
// AsyncWorker: sync_with_peers (network I/O)
// ---------------------------------------------------------------------------

class SyncWorker : public Napi::AsyncWorker {
public:
    explicit SyncWorker(Napi::Env env, Napi::Promise::Deferred d)
        : Napi::AsyncWorker(env), deferred_(d), added_(0) {}

    void Execute() override {
        added_ = sync_with_peers();
    }

    void OnOK() override {
        deferred_.Resolve(Napi::Number::New(Env(), added_));
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    int added_ = 0;
};

// ---------------------------------------------------------------------------
// JS-exposed functions
// ---------------------------------------------------------------------------

Napi::Value CreateChat(const Napi::CallbackInfo& info) {
    auto env     = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    std::string name = info.Length() >= 1 && info[0].IsString()
                       ? info[0].As<Napi::String>().Utf8Value()
                       : "unnamed";
    (new CreateChatWorker(env, deferred, name))->Queue();
    return deferred.Promise();
}

Napi::Value JoinChat(const Napi::CallbackInfo& info) {
    auto env     = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsString()) {
        deferred.Reject(Napi::TypeError::New(env, "Expected (name, userKey, secretId)").Value());
        return deferred.Promise();
    }
    (new JoinChatWorker(env, deferred,
        info[0].As<Napi::String>().Utf8Value(),
        info[1].As<Napi::String>().Utf8Value(),
        info[2].As<Napi::String>().Utf8Value()))->Queue();
    return deferred.Promise();
}

Napi::Value LoadChat(const Napi::CallbackInfo& info) {
    auto env     = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
        deferred.Reject(Napi::TypeError::New(env, "Expected (name, basedir)").Value());
        return deferred.Promise();
    }
    (new LoadChatWorker(env, deferred,
        info[0].As<Napi::String>().Utf8Value(),
        info[1].As<Napi::String>().Utf8Value()))->Queue();
    return deferred.Promise();
}

// Synchronous — fast (file write + AES)
Napi::Value SendMessage(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsString()) {
        Napi::TypeError::New(env, "Expected (chatId, text)").ThrowAsJavaScriptException();
        return env.Null();
    }

    int chatId     = info[0].As<Napi::Number>().Int32Value();
    std::string txt = info[1].As<Napi::String>().Utf8Value();

    std::lock_guard<std::mutex> lock(g_chats_mutex);
    auto it = g_chats.find(chatId);

    auto obj = Napi::Object::New(env);
    if (it == g_chats.end()) {
        obj.Set("success", Napi::Boolean::New(env, false));
        obj.Set("error",   Napi::String::New(env, "Chat not found"));
        return obj;
    }

    // proto_send internally calls db_append → proto_db_wrlock; don't double-lock
    int r = proto_send(&it->second->chat, txt.c_str());

    obj.Set("success", Napi::Boolean::New(env, r == 0));
    if (r != 0) {
        obj.Set("error", Napi::String::New(env, "Not your turn"));
    }
    return obj;
}

// Synchronous — reads from internal cache
Napi::Value GetMessages(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected (chatId)").ThrowAsJavaScriptException();
        return env.Null();
    }

    int chatId = info[0].As<Napi::Number>().Int32Value();

    std::lock_guard<std::mutex> lock(g_chats_mutex);
    auto it = g_chats.find(chatId);
    if (it == g_chats.end()) {
        return Napi::Array::New(env, 0);
    }

    proto_chat* chat = &it->second->chat;

    // proto_list internally acquires proto_db_rdlock; don't double-lock
    const proto_messages* msgs = proto_list(chat);

    if (!msgs || msgs->count == 0) {
        return Napi::Array::New(env, 0);
    }

    auto arr = Napi::Array::New(env, (size_t)msgs->count);
    for (int i = 0; i < msgs->count; i++) {
        bool isMe = (chat->is_initiator && msgs->sender[i] == 0) ||
                    (!chat->is_initiator && msgs->sender[i] == 1);

        auto msg = Napi::Object::New(env);
        msg.Set("text",     Napi::String::New(env, msgs->texts[i]      ? msgs->texts[i]      : ""));
        msg.Set("entityId", Napi::String::New(env, msgs->entity_ids[i] ? msgs->entity_ids[i] : ""));
        msg.Set("sender",   Napi::Number::New(env, msgs->sender[i]));
        msg.Set("isMe",     Napi::Boolean::New(env, isMe));
        arr[i] = msg;
    }
    return arr;
}

// Returns chat metadata: keys, role, state
Napi::Value GetChatInfo(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) {
        return env.Null();
    }

    int chatId = info[0].As<Napi::Number>().Int32Value();
    std::lock_guard<std::mutex> lock(g_chats_mutex);
    auto it = g_chats.find(chatId);
    if (it == g_chats.end()) return env.Null();

    proto_chat* c = &it->second->chat;
    auto obj = Napi::Object::New(env);
    obj.Set("userKey",     Napi::String::New(env, c->user_key));
    obj.Set("secretId",    Napi::String::New(env, c->secret_id));
    obj.Set("isInitiator", Napi::Boolean::New(env, c->is_initiator != 0));
    obj.Set("entityId",    Napi::String::New(env, c->entity_id));
    obj.Set("state",       Napi::Number::New(env, c->state));
    obj.Set("name",        Napi::String::New(env, it->second->name));
    return obj;
}

Napi::Value SaveChat(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsString() || !info[2].IsString()) {
        return Napi::Boolean::New(env, false);
    }

    int chatId      = info[0].As<Napi::Number>().Int32Value();
    std::string name    = info[1].As<Napi::String>().Utf8Value();
    std::string basedir = info[2].As<Napi::String>().Utf8Value();

    std::lock_guard<std::mutex> lock(g_chats_mutex);
    auto it = g_chats.find(chatId);
    if (it == g_chats.end()) return Napi::Boolean::New(env, false);

    int r = proto_save_chat(&it->second->chat, name.c_str(), basedir.c_str());
    return Napi::Boolean::New(env, r == 0);
}

Napi::Value DestroyChat(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) return env.Undefined();

    int chatId = info[0].As<Napi::Number>().Int32Value();
    std::lock_guard<std::mutex> lock(g_chats_mutex);
    auto it = g_chats.find(chatId);
    if (it != g_chats.end()) {
        proto_chat_cleanup(&it->second->chat);
        delete it->second;
        g_chats.erase(it);
    }
    return env.Undefined();
}

// ---------------------------------------------------------------------------
// Sync / network functions
// ---------------------------------------------------------------------------

Napi::Value StartServer(const Napi::CallbackInfo& info) {
    g_sync_port = sync_start_server();
    return Napi::Number::New(info.Env(), g_sync_port);
}

Napi::Value Register(const Napi::CallbackInfo& info) {
    int port = (info.Length() >= 1 && info[0].IsNumber())
               ? info[0].As<Napi::Number>().Int32Value()
               : g_sync_port;
    sync_register(port);
    g_sync_started = true;
    return info.Env().Undefined();
}

Napi::Value Unregister(const Napi::CallbackInfo& info) {
    sync_unregister();
    g_sync_started = false;
    return info.Env().Undefined();
}

Napi::Value AddPeer(const Napi::CallbackInfo& info) {
    if (info.Length() >= 2 && info[0].IsString() && info[1].IsNumber()) {
        sync_add_peer(info[0].As<Napi::String>().Utf8Value().c_str(),
                      info[1].As<Napi::Number>().Int32Value());
    }
    return info.Env().Undefined();
}

Napi::Value SyncWithPeers(const Napi::CallbackInfo& info) {
    auto env     = info.Env();
    auto deferred = Napi::Promise::Deferred::New(env);
    (new SyncWorker(env, deferred))->Queue();
    return deferred.Promise();
}

Napi::Value StartDHT(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    int port = (info.Length() >= 1 && info[0].IsNumber())
               ? info[0].As<Napi::Number>().Int32Value()
               : 0;
    std::string basedir = (info.Length() >= 2 && info[1].IsString())
                          ? info[1].As<Napi::String>().Utf8Value()
                          : ".";
    int r = dht_client_start(port, basedir.c_str());
    g_dht_started = (r == 0);
    return Napi::Number::New(env, r);
}

Napi::Value AddDHTChat(const Napi::CallbackInfo& info) {
    if (info.Length() >= 1 && info[0].IsString()) {
        dht_client_add_chat(info[0].As<Napi::String>().Utf8Value().c_str());
    }
    return info.Env().Undefined();
}

Napi::Value StopDHT(const Napi::CallbackInfo& info) {
    dht_client_stop();
    g_dht_started = false;
    return info.Env().Undefined();
}

Napi::Value GetStatus(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    auto obj = Napi::Object::New(env);
    obj.Set("syncPort",    Napi::Number::New(env, g_sync_port));
    obj.Set("syncStarted", Napi::Boolean::New(env, g_sync_started));
    obj.Set("dhtStarted",  Napi::Boolean::New(env, g_dht_started));
    {
        std::lock_guard<std::mutex> lock(g_chats_mutex);
        obj.Set("chatCount", Napi::Number::New(env, (int)g_chats.size()));
    }
    return obj;
}

// ---------------------------------------------------------------------------
// Module init
// ---------------------------------------------------------------------------

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("createChat",    Napi::Function::New(env, CreateChat));
    exports.Set("joinChat",      Napi::Function::New(env, JoinChat));
    exports.Set("loadChat",      Napi::Function::New(env, LoadChat));
    exports.Set("sendMessage",   Napi::Function::New(env, SendMessage));
    exports.Set("getMessages",   Napi::Function::New(env, GetMessages));
    exports.Set("getChatInfo",   Napi::Function::New(env, GetChatInfo));
    exports.Set("saveChat",      Napi::Function::New(env, SaveChat));
    exports.Set("destroyChat",   Napi::Function::New(env, DestroyChat));
    exports.Set("startServer",   Napi::Function::New(env, StartServer));
    exports.Set("register",      Napi::Function::New(env, Register));
    exports.Set("unregister",    Napi::Function::New(env, Unregister));
    exports.Set("addPeer",       Napi::Function::New(env, AddPeer));
    exports.Set("syncWithPeers", Napi::Function::New(env, SyncWithPeers));
    exports.Set("startDHT",      Napi::Function::New(env, StartDHT));
    exports.Set("addDHTChat",    Napi::Function::New(env, AddDHTChat));
    exports.Set("stopDHT",       Napi::Function::New(env, StopDHT));
    exports.Set("getStatus",     Napi::Function::New(env, GetStatus));
    return exports;
}

NODE_API_MODULE(dcomms_addon, Init)
