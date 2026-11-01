// AI Bridge: lockstep TCP bridge between Geometry Dash (Geode) and an external trainer.
//
// Protocol (newline-terminated text, localhost only):
//   mod -> trainer, every frame:
//     "S px py yvel ground mode flipped percent n dx1 dy1 t1 ... dxn dyn tn"
//   mod -> trainer, when the player dies:
//     "D percent"
//   trainer -> mod, in reply to every S line:
//     "A 0" (release)  "A 1" (hold/click)  "R" (reset level)
//   trainer -> mod, in reply to D:
//     "R"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using sock_t = SOCKET;
    static constexpr sock_t BAD_SOCK = INVALID_SOCKET;
    static void closeSock(sock_t s) { closesocket(s); }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using sock_t = int;
    static constexpr sock_t BAD_SOCK = -1;
    static void closeSock(sock_t s) { close(s); }
#endif

#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace bridge {
    inline std::atomic<sock_t> client{BAD_SOCK};
    inline std::atomic<bool> newClient{false};
    inline std::string buffer;

    inline bool connected() { return client.load() != BAD_SOCK; }

    inline void drop() {
        auto s = client.exchange(BAD_SOCK);
        if (s != BAD_SOCK) closeSock(s);
    }

    inline void poll() {
        if (newClient.exchange(false)) buffer.clear();
    }

    inline bool sendLine(std::string const& line) {
        auto s = client.load();
        if (s == BAD_SOCK) return false;
        std::string out = line + "\n";
        size_t sent = 0;
        while (sent < out.size()) {
            int n = send(s, out.data() + sent, static_cast<int>(out.size() - sent), 0);
            if (n <= 0) { drop(); return false; }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    inline bool recvLine(std::string& out) {
        while (true) {
            auto pos = buffer.find('\n');
            if (pos != std::string::npos) {
                out = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                return true;
            }
            auto s = client.load();
            if (s == BAD_SOCK) return false;
            char tmp[256];
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) { drop(); return false; }
            buffer.append(tmp, static_cast<size_t>(n));
        }
    }

    inline void serverLoop(int port) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv == BAD_SOCK) { log::error("AI Bridge: socket() failed"); return; }

        int yes = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(srv, 1) != 0) {
            log::error("AI Bridge: could not listen on port {}", port);
            return;
        }
        log::info("AI Bridge listening on 127.0.0.1:{}", port);

        while (true) {
            sock_t c = accept(srv, nullptr, nullptr);
            if (c == BAD_SOCK) continue;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
            drop();
            newClient.store(true);
            client.store(c);
            log::info("AI Bridge: trainer connected");
        }
    }

    inline void start(int port) {
        std::thread(serverLoop, port).detach();
    }
}

$on_mod(Loaded) {
    bridge::start(static_cast<int>(Mod::get()->getSettingValue<int64_t>("port")));
}

constexpr float kLookahead = 400.f;
constexpr size_t kMaxObjects = 8;
constexpr float kFixedDt = 1.f / 60.f;

class $modify(AIPlayLayer, PlayLayer) {
    struct Fields {
        bool dead = false;
        bool held = false;
    };

    void setHeld(bool down) {
        auto f = m_fields.self();
        if (down != f->held) {
            this->handleButton(down, 1, true);
            f->held = down;
        }
    }

    std::string buildState() {
        auto p = m_player1;
        float px = p->getPositionX();
        float py = p->getPositionY();

        int mode = 0;
        if (p->m_isShip) mode = 1;
        else if (p->m_isBall) mode = 2;
        else if (p->m_isBird) mode = 3;
        else if (p->m_isDart) mode = 4;
        else if (p->m_isRobot) mode = 5;
        else if (p->m_isSpider) mode = 6;
        else if (p->m_isSwing) mode = 7;

        struct Obs { float dx, dy; int t; };
        std::vector<Obs> obs;
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            float dx = obj->getPositionX() - px;
            if (dx < -30.f || dx > kLookahead) continue;
            int t;
            if (obj->m_objectType == GameObjectType::Hazard) t = 1;
            else if (obj->m_objectType == GameObjectType::Solid) t = 2;
            else continue;
            obs.push_back({dx, obj->getPositionY() - py, t});
        }
        std::sort(obs.begin(), obs.end(), [](Obs const& a, Obs const& b) { return a.dx < b.dx; });
        if (obs.size() > kMaxObjects) obs.resize(kMaxObjects);

        std::ostringstream ss;
        ss << "S " << px << " " << py << " " << p->m_yVelocity << " "
           << (p->m_isOnGround ? 1 : 0) << " " << mode << " "
           << (p->m_isUpsideDown ? 1 : 0) << " " << this->getCurrentPercent() << " "
           << obs.size();
        for (auto const& o : obs) ss << " " << o.dx << " " << o.dy << " " << o.t;
        return ss.str();
    }

    void doReset() {
        setHeld(false);
        m_fields->dead = false;
        this->resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        PlayLayer::destroyPlayer(player, obj);
        if (bridge::connected()) m_fields->dead = true;
    }

    void update(float dt) {
        bridge::poll();
        if (!bridge::connected()) {
            PlayLayer::update(dt);
            return;
        }

        std::string line;

        if (m_fields->dead) {
            if (!bridge::sendLine("D " + std::to_string(this->getCurrentPercent()))) return;
            if (!bridge::recvLine(line)) return;
            doReset();
            return;
        }

        if (!bridge::sendLine(buildState())) { PlayLayer::update(dt); return; }
        if (!bridge::recvLine(line)) { PlayLayer::update(dt); return; }

        if (!line.empty() && line[0] == 'R') {
            doReset();
            return;
        }
        if (line.size() >= 3 && line[0] == 'A') {
            setHeld(line[2] == '1');
        }

        PlayLayer::update(kFixedDt);
    }
};
