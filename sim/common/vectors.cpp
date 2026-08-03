//===========================================================================
// vectors.cpp -- reader for the BCMC test-vector formats
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

std::vector<CellCase> load_cell_vectors(const std::string& path) {
    Tokens                t(path);
    std::vector<CellCase> cases;

    std::string tok;
    int         line = 0;

    // Five numbers per case, on one line by convention but the reader does not
    // depend on that: whitespace is whitespace.
    while (t.next(tok, line)) {
        CellCase c;
        c.line = line;

        std::size_t   used  = 0;
        unsigned long value = 0;
        try {
            value = std::stoul(tok, &used, 10);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used != tok.size()) {
            throw std::runtime_error(t.where(line) + "expected a number, got '" +
                                     tok + "'");
        }
        c.N = static_cast<uint32_t>(value);

        c.weight = t.expect_number();
        c.offset = t.expect_number();
        c.column = t.expect_number();
        c.bit    = t.expect_number();

        if (c.bit > 1) {
            throw std::runtime_error(t.where(c.line) +
                                     "expected bit to be 0 or 1");
        }

        cases.push_back(c);
    }

    if (cases.empty()) {
        throw std::runtime_error(path + ": no cases found");
    }
    return cases;
}

std::vector<MatrixCase> load_matrix_vectors(const std::string& path) {
    Tokens                  t(path);
    std::vector<MatrixCase> cases;

    std::string tok;
    int         line = 0;

    // Keyword-led, so the shape of a case is self-describing and C = 0 needs no
    // special handling: the W and O lines simply carry no numbers.
    while (t.next(tok, line)) {
        if (tok != "N") {
            throw std::runtime_error(t.where(line) +
                                     "expected 'N' at the start of a case, got '" +
                                     tok + "'");
        }
        MatrixCase c;
        c.line = line;
        c.N    = t.expect_number();

        tok = t.expect(line);
        if (tok != "C") {
            throw std::runtime_error(t.where(line) + "expected 'C', got '" +
                                     tok + "'");
        }
        c.C = t.expect_number();

        tok = t.expect(line);
        if (tok != "W") {
            throw std::runtime_error(t.where(line) + "expected 'W', got '" +
                                     tok + "'");
        }
        c.weights.reserve(c.C);
        for (uint32_t i = 0; i < c.C; ++i) c.weights.push_back(t.expect_number());

        tok = t.expect(line);
        if (tok != "O") {
            throw std::runtime_error(t.where(line) + "expected 'O', got '" +
                                     tok + "'");
        }
        c.offsets.reserve(c.C);
        for (uint32_t i = 0; i < c.C; ++i) c.offsets.push_back(t.expect_number());

        c.rows.reserve(c.C);
        for (uint32_t i = 0; i < c.C; ++i) {
            tok = t.expect(line);
            if (tok != "R") {
                throw std::runtime_error(t.where(line) + "expected 'R' for row " +
                                         std::to_string(i) + ", got '" + tok + "'");
            }
            const std::string bits = t.expect(line);
            if (bits.size() != c.N) {
                throw std::runtime_error(t.where(line) + "row " +
                                         std::to_string(i) + " has " +
                                         std::to_string(bits.size()) +
                                         " bits, expected " +
                                         std::to_string(c.N));
            }
            std::vector<uint8_t> row;
            row.reserve(c.N);
            for (char ch : bits) {
                if (ch != '0' && ch != '1') {
                    throw std::runtime_error(t.where(line) +
                                             "row bits must be '0' or '1', got '" +
                                             std::string(1, ch) + "'");
                }
                row.push_back(static_cast<uint8_t>(ch == '1'));
            }
            c.rows.push_back(std::move(row));
        }

        tok = t.expect(line);
        if (tok != "L") {
            throw std::runtime_error(t.where(line) + "expected 'L', got '" +
                                     tok + "'");
        }
        c.occupancy.reserve(c.N);
        for (uint32_t j = 0; j < c.N; ++j) c.occupancy.push_back(t.expect_number());

        cases.push_back(std::move(c));
    }

    if (cases.empty()) {
        throw std::runtime_error(path + ": no cases found");
    }
    return cases;
}

//---------------------------------------------------------------------------
// Bus format
//
// Line oriented rather than token oriented, because an `L` label runs to the
// end of its line. The fields are hexadecimal without an `0x` prefix so that
// Verilog's %h and C++'s std::stoul(.., 16) read the same characters.
//---------------------------------------------------------------------------

std::vector<BusOp> load_bus_vectors(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open vector file: " + path);

    std::vector<BusOp> ops;
    std::string        raw;
    int                line = 0;

    while (std::getline(in, raw)) {
        ++line;

        std::ostringstream where;
        where << path << ":" << line << ": ";

        const std::string::size_type hash = raw.find('#');
        if (hash != std::string::npos) raw.erase(hash);

        std::istringstream fields(raw);
        std::string        op;
        if (!(fields >> op)) continue;  // blank, or comment only

        BusOp o;
        o.line = line;

        if (op == "L") {
            o.kind = BusOp::kLabel;
            std::getline(fields, o.label);
            const std::string::size_type first = o.label.find_first_not_of(" \t");
            o.label = (first == std::string::npos) ? "" : o.label.substr(first);
            ops.push_back(std::move(o));
            continue;
        }

        if (op == "Z") {
            o.kind = BusOp::kReset;
            ops.push_back(std::move(o));
            continue;
        }

        if (op != "R" && op != "W" && op != "P") {
            throw std::runtime_error(where.str() + "unknown op '" + op + "'");
        }

        std::string adr, sel, data, exp, rdata;
        if (!(fields >> adr >> sel >> data >> exp >> rdata)) {
            throw std::runtime_error(where.str() + "expected: op adr sel data exp rdata");
        }

        auto hex = [&where](const std::string& tok) {
            std::size_t   used  = 0;
            unsigned long value = 0;
            try {
                value = std::stoul(tok, &used, 16);
            } catch (const std::exception&) {
                throw std::runtime_error(where.str() + "expected hex, got '" + tok + "'");
            }
            if (used != tok.size()) {
                throw std::runtime_error(where.str() + "expected hex, got '" + tok + "'");
            }
            return static_cast<uint32_t>(value);
        };

        o.kind  = (op == "R") ? BusOp::kRead : (op == "W") ? BusOp::kWrite : BusOp::kPoll;
        o.adr   = hex(adr);
        o.sel   = hex(sel);
        o.data  = hex(data);
        o.rdata = hex(rdata);

        if (exp == "ACK") {
            o.err = false;
        } else if (exp == "ERR") {
            o.err = true;
        } else {
            throw std::runtime_error(where.str() + "expected ACK or ERR, got '" + exp + "'");
        }

        // A poll is a wait, not a refusal: there is nothing sensible for it to
        // mean if the access it repeats is expected to fail.
        if (o.kind == BusOp::kPoll && o.err) {
            throw std::runtime_error(where.str() + "a P op cannot expect ERR");
        }

        ops.push_back(std::move(o));
    }

    if (ops.empty()) {
        throw std::runtime_error(path + ": no cases found");
    }
    return ops;
}

}  // namespace bcmc

