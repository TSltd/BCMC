//===========================================================================
// vectors.cpp -- reader for the BCMC test-vector format
//===========================================================================

#include "vectors.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bcmc {
namespace {

// A whitespace-delimited token stream that strips '#' comments and remembers
// which line each token came from.
class Tokens {
  public:
    explicit Tokens(const std::string& path) : path_(path), in_(path) {
        if (!in_) {
            throw std::runtime_error("cannot open vector file: " + path);
        }
    }

    // Returns false at end of file.
    bool next(std::string& tok, int& line) {
        for (;;) {
            if (pos_ >= buf_.size()) {
                if (!std::getline(in_, buf_)) return false;
                ++line_;
                pos_ = 0;
                const std::string::size_type hash = buf_.find('#');
                if (hash != std::string::npos) buf_.erase(hash);
            }
            while (pos_ < buf_.size() && isspace_(buf_[pos_])) ++pos_;
            if (pos_ >= buf_.size()) {
                buf_.clear();
                continue;
            }
            const std::string::size_type start = pos_;
            while (pos_ < buf_.size() && !isspace_(buf_[pos_])) ++pos_;
            tok  = buf_.substr(start, pos_ - start);
            line = line_;
            return true;
        }
    }

    // Like next(), but end of file is an error.
    std::string expect(int& line) {
        std::string tok;
        if (!next(tok, line)) {
            throw std::runtime_error(where(line_) + "unexpected end of file");
        }
        return tok;
    }

    uint32_t expect_number() {
        int               line = 0;
        const std::string tok  = expect(line);
        std::size_t       used = 0;
        unsigned long     value = 0;
        try {
            value = std::stoul(tok, &used, 10);
        } catch (const std::exception&) {
            throw std::runtime_error(where(line) + "expected a number, got '" +
                                     tok + "'");
        }
        if (used != tok.size()) {
            throw std::runtime_error(where(line) + "expected a number, got '" +
                                     tok + "'");
        }
        return static_cast<uint32_t>(value);
    }

    std::string where(int line) const {
        std::ostringstream os;
        os << path_ << ":" << line << ": ";
        return os.str();
    }

    int line() const { return line_; }

  private:
    static bool isspace_(char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
               ch == '\v' || ch == '\f';
    }

    std::string             path_;
    std::ifstream           in_;
    std::string             buf_;
    std::string::size_type  pos_  = 0;
    int                     line_ = 0;
};

}  // namespace

std::vector<Case> load_vectors(const std::string& path) {
    Tokens            t(path);
    std::vector<Case> cases;

    std::string tok;
    int         line = 0;

    while (t.next(tok, line)) {
        if (tok != "N") {
            throw std::runtime_error(t.where(line) +
                                     "expected 'N' at the start of a case, got '" +
                                     tok + "'");
        }
        Case c;
        c.line = line;
        c.N    = t.expect_number();

        tok = t.expect(line);
        if (tok != "C") {
            throw std::runtime_error(t.where(line) + "expected 'C', got '" +
                                     tok + "'");
        }
        c.C = t.expect_number();

        c.weights.reserve(c.C);
        for (uint32_t i = 0; i < c.C; ++i) c.weights.push_back(t.expect_number());

        tok = t.expect(line);
        if (tok != "---") {
            throw std::runtime_error(t.where(line) +
                                     "expected '---' between weights and offsets, got '" +
                                     tok + "'");
        }

        c.offsets.reserve(c.C);
        for (uint32_t i = 0; i < c.C; ++i) c.offsets.push_back(t.expect_number());

        cases.push_back(std::move(c));
    }

    if (cases.empty()) {
        throw std::runtime_error(path + ": no cases found");
    }
    return cases;
}

}  // namespace bcmc
