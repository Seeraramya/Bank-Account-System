// main.cpp - tiny dependency-free HTTP server for Nova Bank.
// Serves index.html at "/" and the JSON API used by the page.
// Usage: nova_bank [port] [bind-address]     (defaults: 8080, 127.0.0.1)
#include "bank.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  #define CLOSESOCK closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <signal.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  using sock_t = int;
  constexpr sock_t INVALID_SOCKET = -1;
  #define CLOSESOCK close
#endif

namespace {

struct Request {
    std::string method, path, body;
};

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Look for index.html in the working dir, then one level up (handy for build/ folders).
std::string loadIndex() {
    for (const char* p : {"index.html", "../index.html", "../../index.html"}) {
        std::string s = readFile(p);
        if (!s.empty()) return s;
    }
    return "";
}

bool readRequest(sock_t s, Request& rq) {
    std::string buf;
    char tmp[4096];
    size_t hdrEnd;
    while ((hdrEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > 16384) return false;
        long long n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
    std::string head = buf.substr(0, hdrEnd);
    std::istringstream line(head.substr(0, head.find("\r\n")));
    std::string target, version;
    line >> rq.method >> target >> version;
    if (rq.method.empty() || target.empty()) return false;
    rq.path = target.substr(0, target.find('?'));

    std::string lower = head;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t len = 0;
    size_t cl = lower.find("content-length:");
    if (cl != std::string::npos) len = static_cast<size_t>(std::strtoull(lower.c_str() + cl + 15, nullptr, 10));
    if (len > 65536) return false;

    size_t start = hdrEnd + 4;
    while (buf.size() - start < len) {
        long long n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
    rq.body = buf.substr(start, len);
    return true;
}

void sendAll(sock_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        long long n = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

const char* statusText(int c) {
    switch (c) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        default:  return "Internal Server Error";
    }
}

void respond(sock_t s, int status, const std::string& type, const std::string& body) {
    std::string h = "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n" +
                    "Content-Type: " + type + "\r\n" +
                    "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                    "Cache-Control: no-store\r\n" +
                    "X-Content-Type-Options: nosniff\r\n" +
                    "Connection: close\r\n\r\n";
    sendAll(s, h + body);
}

void respondJson(sock_t s, const Result& r) {
    if (r.status == 200)
        respond(s, 200, "application/json", "{\"ok\":true,\"data\":" + r.body + "}");
    else
        respond(s, r.status, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(r.body) + "\"}");
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                 std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::map<std::string, std::string> parseForm(const std::string& body) {
    std::map<std::string, std::string> m;
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        m[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
    return m;
}

bool toId(const std::string& s, int& id) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    id = std::stoi(s);
    return true;
}

Result bad(const std::string& m) { return {400, m}; }

Result route(Bank& bank, const Request& rq) {
    const std::string& p = rq.path;
    if (rq.method == "GET") {
        if (p == "/api/accounts") return bank.list();
        const std::string pre = "/api/accounts/", suf = "/transactions";
        if (p.size() > pre.size() + suf.size() && p.compare(0, pre.size(), pre) == 0 &&
            p.compare(p.size() - suf.size(), suf.size(), suf) == 0) {
            int id;
            if (!toId(p.substr(pre.size(), p.size() - pre.size() - suf.size()), id)) return {404, "Account not found."};
            return bank.transactions(id);
        }
        return {404, "Not found."};
    }
    if (rq.method != "POST") return {405, "Method not allowed."};

    auto f = parseForm(rq.body);
    long long cents = 0;
    int id = 0;

    if (p == "/api/accounts") {
        long long initial = 0;
        if (!f["initial"].empty() && !parseMoney(f["initial"], initial)) return bad("Enter a valid opening deposit, e.g. 500.00");
        return bank.open(f["name"], initial);
    }
    if (p == "/api/deposit" || p == "/api/withdraw") {
        if (!toId(f["id"], id)) return bad("Choose an account.");
        if (!parseMoney(f["amount"], cents)) return bad("Enter a valid amount, e.g. 500.00");
        return p == "/api/deposit" ? bank.deposit(id, cents) : bank.withdraw(id, cents);
    }
    if (p == "/api/transfer") {
        int from, to;
        if (!toId(f["from"], from) || !toId(f["to"], to)) return bad("Choose both accounts.");
        if (!parseMoney(f["amount"], cents)) return bad("Enter a valid amount, e.g. 500.00");
        return bank.transfer(from, to, cents);
    }
    return {404, "Not found."};
}

void handle(sock_t s, Bank& bank) {
#ifdef _WIN32
    DWORD tmo = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
#else
    timeval tv{5, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    Request rq;
    if (readRequest(s, rq)) {
        try {
            if (rq.method == "GET" && (rq.path == "/" || rq.path == "/index.html")) {
                std::string page = loadIndex();  // re-read each time so edits show on refresh
                if (page.empty()) respond(s, 404, "text/plain; charset=utf-8", "index.html not found next to the server.");
                else respond(s, 200, "text/html; charset=utf-8", page);
            } else if (rq.method == "GET" && rq.path == "/favicon.ico") {
                respond(s, 404, "text/plain", "");
            } else {
                respondJson(s, route(bank, rq));
            }
        } catch (...) {
            respondJson(s, {500, "Something went wrong on the server."});
        }
    }
    CLOSESOCK(s);
}

}  // namespace

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    std::string host = argc > 2 ? argv[2] : "127.0.0.1";
    if (port <= 0 || port > 65535) { std::cerr << "Invalid port.\n"; return 1; }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::cerr << "WSAStartup failed.\n"; return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { std::cerr << "Cannot create socket.\n"; return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) { std::cerr << "Invalid bind address.\n"; return 1; }
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(srv, 64) != 0) {
        std::cerr << "Cannot listen on " << host << ":" << port << " (is the port already in use?)\n";
        return 1;
    }

    Bank bank("bank.db");
    std::cout << "Nova Bank running -> http://" << (host == "0.0.0.0" ? "localhost" : host) << ":" << port << "\n"
              << "Press Ctrl+C to stop.\n";

    for (;;) {
        sock_t c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread([c, &bank] { handle(c, bank); }).detach();
    }
}