// main.cpp - HTTP server for Nova Bank
// Serves index.html and the JSON API.
//
// Local usage:
//   g++ -std=c++17 main.cpp -o main -lws2_32
//   .\main.exe
//
// Local URL:
//   http://127.0.0.1:8080
//
// Render:
//   Automatically uses the PORT environment variable
//   and listens on 0.0.0.0

#include "bank.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include <cctype>

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


// ============================================================
// REQUEST STRUCTURE
// ============================================================

struct Request {
    std::string method;
    std::string path;
    std::string body;
};


// ============================================================
// READ FILE
// ============================================================

std::string readFile(const std::string& path) {

    std::ifstream f(path, std::ios::binary);

    if (!f)
        return "";

    std::stringstream ss;

    ss << f.rdbuf();

    return ss.str();
}


// ============================================================
// LOAD INDEX.HTML
// ============================================================

std::string loadIndex() {

    const char* paths[] = {
        "index.html",
        "../index.html",
        "../../index.html"
    };

    for (const char* p : paths) {

        std::string s = readFile(p);

        if (!s.empty())
            return s;
    }

    return "";
}


// ============================================================
// READ HTTP REQUEST
// ============================================================

bool readRequest(sock_t s, Request& rq) {

    std::string buf;

    char tmp[4096];

    size_t hdrEnd;

    while ((hdrEnd = buf.find("\r\n\r\n")) == std::string::npos) {

        if (buf.size() > 16384)
            return false;

        long long n = recv(
            s,
            tmp,
            sizeof tmp,
            0
        );

        if (n <= 0)
            return false;

        buf.append(
            tmp,
            static_cast<size_t>(n)
        );
    }


    std::string head =
        buf.substr(0, hdrEnd);


    std::istringstream line(
        head.substr(
            0,
            head.find("\r\n")
        )
    );


    std::string target;
    std::string version;


    line >> rq.method
         >> target
         >> version;


    if (rq.method.empty() ||
        target.empty()) {

        return false;
    }


    // Remove query string

    rq.path =
        target.substr(
            0,
            target.find('?')
        );


    // Convert headers to lowercase

    std::string lower = head;

    for (auto& c : lower) {

        c = static_cast<char>(
            std::tolower(
                static_cast<unsigned char>(c)
            )
        );
    }


    // Content-Length

    size_t len = 0;

    size_t cl =
        lower.find("content-length:");


    if (cl != std::string::npos) {

        len = static_cast<size_t>(
            std::strtoull(
                lower.c_str() + cl + 15,
                nullptr,
                10
            )
        );
    }


    if (len > 65536)
        return false;


    size_t start =
        hdrEnd + 4;


    while (buf.size() - start < len) {

        long long n =
            recv(
                s,
                tmp,
                sizeof tmp,
                0
            );


        if (n <= 0)
            return false;


        buf.append(
            tmp,
            static_cast<size_t>(n)
        );
    }


    rq.body =
        buf.substr(start, len);


    return true;
}


// ============================================================
// SEND ALL DATA
// ============================================================

void sendAll(
    sock_t s,
    const std::string& data
) {

    size_t off = 0;


    while (off < data.size()) {

        long long n =
            send(
                s,
                data.data() + off,
                static_cast<int>(
                    data.size() - off
                ),
                0
            );


        if (n <= 0)
            return;


        off +=
            static_cast<size_t>(n);
    }
}


// ============================================================
// HTTP STATUS TEXT
// ============================================================

const char* statusText(int c) {

    switch (c) {

        case 200:
            return "OK";

        case 400:
            return "Bad Request";

        case 404:
            return "Not Found";

        case 405:
            return "Method Not Allowed";

        case 500:
            return "Internal Server Error";

        default:
            return "Internal Server Error";
    }
}


// ============================================================
// SEND HTTP RESPONSE
// ============================================================

void respond(
    sock_t s,
    int status,
    const std::string& type,
    const std::string& body
) {

    std::string h =
        "HTTP/1.1 " +
        std::to_string(status) +
        " " +
        statusText(status) +
        "\r\n" +

        "Content-Type: " +
        type +
        "\r\n" +

        "Content-Length: " +
        std::to_string(body.size()) +
        "\r\n" +

        // Allow Vercel frontend to communicate
        // with Render backend.
        "Access-Control-Allow-Origin: *\r\n" +

        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n" +

        "Access-Control-Allow-Headers: Content-Type\r\n" +

        "Cache-Control: no-store\r\n" +

        "X-Content-Type-Options: nosniff\r\n" +

        "Connection: close\r\n\r\n";


    sendAll(
        s,
        h + body
    );
}


// ============================================================
// JSON RESPONSE
// ============================================================

void respondJson(
    sock_t s,
    const Result& r
) {

    if (r.status == 200) {

        respond(
            s,
            200,
            "application/json",
            "{\"ok\":true,\"data\":" +
            r.body +
            "}"
        );

    } else {

        respond(
            s,
            r.status,
            "application/json",
            "{\"ok\":false,\"error\":\"" +
            jsonEscape(r.body) +
            "\"}"
        );
    }
}


// ============================================================
// URL DECODE
// ============================================================

std::string urlDecode(
    const std::string& s
) {

    std::string out;


    for (size_t i = 0;
         i < s.size();
         ++i) {


        if (s[i] == '+') {

            out += ' ';
        }


        else if (
            s[i] == '%' &&
            i + 2 < s.size() &&
            std::isxdigit(
                static_cast<unsigned char>(
                    s[i + 1]
                )
            ) &&
            std::isxdigit(
                static_cast<unsigned char>(
                    s[i + 2]
                )
            )
        ) {

            out += static_cast<char>(
                std::stoi(
                    s.substr(i + 1, 2),
                    nullptr,
                    16
                )
            );

            i += 2;
        }


        else {

            out += s[i];
        }
    }


    return out;
}


// ============================================================
// PARSE FORM DATA
// ============================================================

std::map<std::string, std::string>
parseForm(const std::string& body) {

    std::map<std::string, std::string> m;

    std::stringstream ss(body);

    std::string pair;


    while (std::getline(ss, pair, '&')) {

        size_t eq =
            pair.find('=');


        if (eq == std::string::npos)
            continue;


        m[
            urlDecode(
                pair.substr(0, eq)
            )
        ] =
            urlDecode(
                pair.substr(eq + 1)
            );
    }


    return m;
}


// ============================================================
// CONVERT STRING TO ACCOUNT ID
// ============================================================

bool toId(
    const std::string& s,
    int& id
) {

    if (s.empty() ||
        s.size() > 9) {

        return false;
    }


    for (char c : s) {

        if (!std::isdigit(
                static_cast<unsigned char>(c)
            )) {

            return false;
        }
    }


    id = std::stoi(s);

    return true;
}


// ============================================================
// BAD REQUEST
// ============================================================

Result bad(
    const std::string& m
) {

    return {
        400,
        m
    };
}


// ============================================================
// API ROUTES
// ============================================================

Result route(
    Bank& bank,
    const Request& rq
) {

    const std::string& p =
        rq.path;


    // ========================================================
    // GET REQUESTS
    // ========================================================

    if (rq.method == "GET") {


        // Get all accounts

        if (p == "/api/accounts") {

            return bank.list();
        }


        // Get account transactions

        const std::string pre =
            "/api/accounts/";

        const std::string suf =
            "/transactions";


        if (
            p.size() >
                pre.size() +
                suf.size() &&

            p.compare(
                0,
                pre.size(),
                pre
            ) == 0 &&

            p.compare(
                p.size() - suf.size(),
                suf.size(),
                suf
            ) == 0
        ) {

            int id;


            if (
                !toId(
                    p.substr(
                        pre.size(),
                        p.size() -
                        pre.size() -
                        suf.size()
                    ),
                    id
                )
            ) {

                return {
                    404,
                    "Account not found."
                };
            }


            return bank.transactions(id);
        }


        return {
            404,
            "Not found."
        };
    }


    // ========================================================
    // ONLY POST AFTER THIS POINT
    // ========================================================

    if (rq.method != "POST") {

        return {
            405,
            "Method not allowed."
        };
    }


    auto f =
        parseForm(rq.body);


    long long cents = 0;

    int id = 0;


    // ========================================================
    // CREATE ACCOUNT
    // ========================================================

    if (p == "/api/accounts") {

        long long initial = 0;


        if (
            !f["initial"].empty() &&
            !parseMoney(
                f["initial"],
                initial
            )
        ) {

            return bad(
                "Enter a valid opening deposit, e.g. 500.00"
            );
        }


        return bank.open(
            f["name"],
            initial
        );
    }


    // ========================================================
    // DEPOSIT / WITHDRAW
    // ========================================================

    if (
        p == "/api/deposit" ||
        p == "/api/withdraw"
    ) {


        if (
            !toId(
                f["id"],
                id
            )
        ) {

            return bad(
                "Choose an account."
            );
        }


        if (
            !parseMoney(
                f["amount"],
                cents
            )
        ) {

            return bad(
                "Enter a valid amount, e.g. 500.00"
            );
        }


        if (p == "/api/deposit") {

            return bank.deposit(
                id,
                cents
            );

        } else {

            return bank.withdraw(
                id,
                cents
            );
        }
    }


    // ========================================================
    // TRANSFER
    // ========================================================

    if (p == "/api/transfer") {

        int from;
        int to;


        if (
            !toId(
                f["from"],
                from
            ) ||

            !toId(
                f["to"],
                to
            )
        ) {

            return bad(
                "Choose both accounts."
            );
        }


        if (
            !parseMoney(
                f["amount"],
                cents
            )
        ) {

            return bad(
                "Enter a valid amount, e.g. 500.00"
            );
        }


        return bank.transfer(
            from,
            to,
            cents
        );
    }


    return {
        404,
        "Not found."
    };
}


// ============================================================
// HANDLE CLIENT
// ============================================================

void handle(
    sock_t s,
    Bank& bank
) {


#ifdef _WIN32

    DWORD tmo = 5000;

    setsockopt(
        s,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&tmo),
        sizeof tmo
    );

#else

    timeval tv{5, 0};

    setsockopt(
        s,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &tv,
        sizeof tv
    );

#endif


    Request rq;


    if (readRequest(s, rq)) {

        try {


            // =================================================
            // CORS PREFLIGHT REQUEST
            // =================================================

            if (rq.method == "OPTIONS") {

                respond(
                    s,
                    200,
                    "text/plain",
                    ""
                );
            }


            // =================================================
            // SERVE HOME PAGE
            // =================================================

            else if (
                rq.method == "GET" &&
                (
                    rq.path == "/" ||
                    rq.path == "/index.html"
                )
            ) {

                std::string page =
                    loadIndex();


                if (page.empty()) {

                    respond(
                        s,
                        404,
                        "text/plain; charset=utf-8",
                        "index.html not found next to the server."
                    );

                } else {

                    respond(
                        s,
                        200,
                        "text/html; charset=utf-8",
                        page
                    );
                }
            }


            // =================================================
            // FAVICON
            // =================================================

            else if (
                rq.method == "GET" &&
                rq.path == "/favicon.ico"
            ) {

                respond(
                    s,
                    404,
                    "text/plain",
                    ""
                );
            }


            // =================================================
            // API
            // =================================================

            else {

                respondJson(
                    s,
                    route(bank, rq)
                );
            }


        } catch (...) {

            respondJson(
                s,
                {
                    500,
                    "Something went wrong on the server."
                }
            );
        }
    }


    CLOSESOCK(s);
}

} // namespace


// ============================================================
// MAIN
// ============================================================

int main(
    int argc,
    char** argv
) {

    // ========================================================
    // PORT
    // ========================================================

    int port;


    // Render provides PORT environment variable

    const char* envPort =
        std::getenv("PORT");


    if (envPort) {

        port =
            std::atoi(envPort);

    }

    // Local command-line port

    else if (argc > 1) {

        port =
            std::atoi(argv[1]);

    }

    // Default local port

    else {

        port = 8080;
    }


    // ========================================================
    // HOST
    // ========================================================

    std::string host;


    if (argc > 2) {

        host = argv[2];

    } else {

        // IMPORTANT:
        // 0.0.0.0 allows Render/public connections

        host = "0.0.0.0";
    }


    // ========================================================
    // VALIDATE PORT
    // ========================================================

    if (
        port <= 0 ||
        port > 65535
    ) {

        std::cerr
            << "Invalid port.\n";

        return 1;
    }


    // ========================================================
    // WINDOWS SOCKET INITIALIZATION
    // ========================================================

#ifdef _WIN32

    WSADATA wsa;


    if (
        WSAStartup(
            MAKEWORD(2, 2),
            &wsa
        ) != 0
    ) {

        std::cerr
            << "WSAStartup failed.\n";

        return 1;
    }

#else

    // Prevent broken-pipe termination on Linux

    signal(
        SIGPIPE,
        SIG_IGN
    );

#endif


    // ========================================================
    // CREATE SOCKET
    // ========================================================

    sock_t srv =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );


    if (
        srv == INVALID_SOCKET
    ) {

        std::cerr
            << "Cannot create socket.\n";

        return 1;
    }


    // ========================================================
    // REUSE ADDRESS
    // ========================================================

    int yes = 1;


#ifdef _WIN32

    setsockopt(
        srv,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&yes),
        sizeof yes
    );

#else

    setsockopt(
        srv,
        SOL_SOCKET,
        SO_REUSEADDR,
        &yes,
        sizeof yes
    );

#endif


    // ========================================================
    // SERVER ADDRESS
    // ========================================================

    sockaddr_in addr{};


    addr.sin_family =
        AF_INET;


    addr.sin_port =
        htons(
            static_cast<unsigned short>(
                port
            )
        );


    if (
        inet_pton(
            AF_INET,
            host.c_str(),
            &addr.sin_addr
        ) != 1
    ) {

        std::cerr
            << "Invalid bind address.\n";

        CLOSESOCK(srv);

        return 1;
    }


    // ========================================================
    // BIND
    // ========================================================

    if (
        bind(
            srv,
            reinterpret_cast<sockaddr*>(&addr),
            sizeof addr
        ) != 0
    ) {

        std::cerr
            << "Cannot bind to "
            << host
            << ":"
            << port
            << "\n";

        CLOSESOCK(srv);

        return 1;
    }


    // ========================================================
    // LISTEN
    // ========================================================

    if (
        listen(
            srv,
            64
        ) != 0
    ) {

        std::cerr
            << "Cannot listen on "
            << host
            << ":"
            << port
            << "\n";

        CLOSESOCK(srv);

        return 1;
    }


    // ========================================================
    // BANK DATABASE
    // ========================================================

    Bank bank(
        "bank.db"
    );


    // ========================================================
    // START MESSAGE
    // ========================================================

    std::cout
        << "Nova Bank running -> http://"
        << (
            host == "0.0.0.0"
                ? "localhost"
                : host
        )
        << ":"
        << port
        << "\n";


    std::cout
        << "Press Ctrl+C to stop.\n";


    // ========================================================
    // ACCEPT CLIENT CONNECTIONS
    // ========================================================

    for (;;) {

        sock_t c =
            accept(
                srv,
                nullptr,
                nullptr
            );


        if (
            c == INVALID_SOCKET
        ) {

            continue;
        }


        std::thread(
            [c, &bank] {

                handle(
                    c,
                    bank
                );

            }
        ).detach();
    }


    return 0;
}
