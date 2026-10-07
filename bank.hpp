// bank.hpp - core banking engine (header-only). Money is stored as integer cents.
#pragma once
#include <cctype>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// status 200 -> body is a JSON value; anything else -> body is an error message
struct Result {
    int status;
    std::string body;
};

inline std::string jsonEscape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\r': r += "\\r";  break;
            case '\t': r += "\\t";  break;
            default:   r += (c < 0x20) ? ' ' : static_cast<char>(c);
        }
    }
    return r;
}

// "123", "123.4", "123.45" -> cents. No floating point involved.
inline bool parseMoney(std::string s, long long& cents) {
    size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
    if (b == std::string::npos) return false;
    s = s.substr(b, e - b + 1);
    if (s.size() > 15) return false;
    size_t dot = s.find('.');
    std::string ip = s.substr(0, dot);
    std::string fp = (dot == std::string::npos) ? "" : s.substr(dot + 1);
    if (ip.empty() && fp.empty()) return false;
    if (fp.size() > 2) return false;
    for (char c : ip) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    for (char c : fp) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    while (fp.size() < 2) fp += '0';
    long long whole = ip.empty() ? 0 : std::stoll(ip);
    cents = whole * 100 + std::stoll(fp);
    return true;
}

class Bank {
    struct Txn {
        std::string type;
        long long amount;
        long long balanceAfter;
        long long ts;
        std::string note;
    };
    struct Account {
        int id;
        std::string name;
        long long balance;
        std::vector<Txn> txns;
    };

    static constexpr long long kMaxBalance = 100000000000000LL;  // 1e14 cents

    std::map<int, Account> accounts_;
    int nextId_ = 1001;
    std::string db_;
    std::mutex m_;

    static Result fail(int status, const std::string& msg) { return {status, msg}; }
    static Result ok(const std::string& json) { return {200, json}; }

    static std::string accJson(const Account& a) {
        return "{\"id\":" + std::to_string(a.id) + ",\"name\":\"" + jsonEscape(a.name) +
               "\",\"balance_cents\":" + std::to_string(a.balance) + "}";
    }

    static void record(Account& a, const std::string& type, long long amount, const std::string& note) {
        a.txns.push_back({type, amount, a.balance, static_cast<long long>(std::time(nullptr)), note});
    }

    // ---- persistence: tab-separated text file ----
    void save() {
        std::ofstream f(db_, std::ios::trunc);
        if (!f) return;
        f << "N\t" << nextId_ << "\n";
        for (const auto& kv : accounts_) {
            const Account& a = kv.second;
            f << "A\t" << a.id << "\t" << a.name << "\t" << a.balance << "\n";
            for (const auto& t : a.txns)
                f << "T\t" << a.id << "\t" << t.type << "\t" << t.amount << "\t"
                  << t.balanceAfter << "\t" << t.ts << "\t" << t.note << "\n";
        }
    }

    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        std::stringstream ss(s);
        while (std::getline(ss, cur, '\t')) out.push_back(cur);
        return out;
    }

    void load() {
        std::ifstream f(db_);
        std::string line;
        while (std::getline(f, line)) {
            auto p = split(line);
            if (p.empty()) continue;
            try {
                if (p[0] == "N" && p.size() >= 2) {
                    nextId_ = std::stoi(p[1]);
                } else if (p[0] == "A" && p.size() >= 4) {
                    Account a{std::stoi(p[1]), p[2], std::stoll(p[3]), {}};
                    accounts_[a.id] = a;
                } else if (p[0] == "T" && p.size() >= 7) {
                    auto it = accounts_.find(std::stoi(p[1]));
                    if (it != accounts_.end())
                        it->second.txns.push_back({p[2], std::stoll(p[3]), std::stoll(p[4]), std::stoll(p[5]), p[6]});
                }
            } catch (...) { /* skip a corrupt line */ }
        }
    }

    Result change(int id, long long cents, bool deposit) {
        std::lock_guard<std::mutex> lk(m_);
        if (cents <= 0) return fail(400, "Amount must be greater than zero.");
        auto it = accounts_.find(id);
        if (it == accounts_.end()) return fail(404, "Account not found.");
        Account& a = it->second;
        if (deposit && a.balance > kMaxBalance - cents) return fail(400, "Amount is too large.");
        if (!deposit && a.balance < cents) return fail(400, "Insufficient funds.");
        a.balance += deposit ? cents : -cents;
        record(a, deposit ? "DEPOSIT" : "WITHDRAWAL", cents, deposit ? "Cash deposit" : "Cash withdrawal");
        save();
        return ok(accJson(a));
    }

public:
    explicit Bank(std::string dbPath) : db_(std::move(dbPath)) { load(); }

    Result open(const std::string& rawName, long long initial) {
        std::lock_guard<std::mutex> lk(m_);
        std::string n = rawName;
        for (auto& c : n) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        size_t b = n.find_first_not_of(' '), e = n.find_last_not_of(' ');
        if (b == std::string::npos) return fail(400, "Account holder name is required.");
        n = n.substr(b, e - b + 1);
        if (n.size() > 60) return fail(400, "Name is too long (max 60 characters).");
        if (initial < 0 || initial > kMaxBalance) return fail(400, "Invalid opening deposit.");
        Account a{nextId_++, n, initial, {}};
        if (initial > 0) record(a, "DEPOSIT", initial, "Opening deposit");
        accounts_[a.id] = a;
        save();
        return ok(accJson(a));
    }

    Result deposit(int id, long long cents)  { return change(id, cents, true); }
    Result withdraw(int id, long long cents) { return change(id, cents, false); }

    Result transfer(int from, int to, long long cents) {
        std::lock_guard<std::mutex> lk(m_);
        if (cents <= 0) return fail(400, "Amount must be greater than zero.");
        if (from == to) return fail(400, "Choose two different accounts.");
        auto f = accounts_.find(from), t = accounts_.find(to);
        if (f == accounts_.end() || t == accounts_.end()) return fail(404, "Account not found.");
        Account& A = f->second;
        Account& B = t->second;
        if (A.balance < cents) return fail(400, "Insufficient funds.");
        if (B.balance > kMaxBalance - cents) return fail(400, "Amount is too large.");
        A.balance -= cents;
        B.balance += cents;
        record(A, "TRANSFER_OUT", cents, "To #" + std::to_string(B.id) + " (" + B.name + ")");
        record(B, "TRANSFER_IN", cents, "From #" + std::to_string(A.id) + " (" + A.name + ")");
        save();
        return ok("{\"from\":" + accJson(A) + ",\"to\":" + accJson(B) + "}");
    }

    Result list() {
        std::lock_guard<std::mutex> lk(m_);
        std::string s = "[";
        for (const auto& kv : accounts_) {
            if (s.size() > 1) s += ",";
            s += accJson(kv.second);
        }
        return ok(s + "]");
    }

    Result transactions(int id) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = accounts_.find(id);
        if (it == accounts_.end()) return fail(404, "Account not found.");
        const auto& v = it->second.txns;
        std::string s = "[";
        for (size_t i = v.size(); i-- > 0;) {  // newest first
            const Txn& t = v[i];
            if (s.size() > 1) s += ",";
            s += "{\"type\":\"" + jsonEscape(t.type) + "\",\"amount_cents\":" + std::to_string(t.amount) +
                 ",\"balance_after_cents\":" + std::to_string(t.balanceAfter) +
                 ",\"ts\":" + std::to_string(t.ts) + ",\"note\":\"" + jsonEscape(t.note) + "\"}";
        }
        return ok(s + "]");
    }
};