#include "indexer.h"
#include "recursion.h"

#include <gflags/gflags.h>

#include <list>

#include <stdarg.h>

DEFINE_int32(debug_index, 0, "Debug the index query generator.");
static void __index_debug(const char *format, ...)
    __attribute__((format (printf, 1, 2)));

#define debug(lvl, fmt, ...) do {                \
        if (FLAGS_debug_index >= lvl)            \
            __index_debug(fmt, ## __VA_ARGS__);  \
    } while(0)

static void __index_debug(const char *format, ...) {
    if (!FLAGS_debug_index)
        return;
    va_list ap;

    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
}

using namespace re2;
using namespace std;

const unsigned kMinWeight = 16;
const int kMaxWidth       = 32;
const int kMaxRecursion   = 10;
const int kMaxNodes       = (1 << 24);

namespace {
    static IndexKey::Stats null_stats;
};

IndexKey::Stats::Stats ()
    : selectivity_(1.0), depth_(0), nodes_(1), tail_paths_(1) {
}

IndexKey::Stats IndexKey::Stats::insert(const value_type& val) const {
    Stats out(*this);
    if (out.selectivity_ == 1.0) {
        out.selectivity_ = 0.0;
        out.tail_paths_  = 0;
    }

    const Stats& rstats = val.second ? val.second->stats() : null_stats;

    out.selectivity_ += (val.first.second - val.first.first + 1)/128. * rstats.selectivity_;
    out.depth_ = max(depth_, rstats.depth_ + 1);
    out.nodes_ += (val.first.second - val.first.first + 1) * rstats.nodes_;
    if (!val.second)
        out.tail_paths_ += (val.first.second - val.first.first + 1);

    return out;
}

IndexKey::Stats IndexKey::Stats::concat(const IndexKey::Stats& rhs) const {
    Stats out(*this);
    out.selectivity_ *= rhs.selectivity_;
    out.depth_ += rhs.depth_;
    out.nodes_ += tail_paths_ * rhs.nodes_;
    out.tail_paths_ *= rhs.tail_paths_;

    return out;
}

void IndexKey::insert(const value_type& val) {
    stats_ = stats_.insert(val);

    iterator it = edges_.insert(val).first;
    if (val.second) {
        tails_.splice(tails_.end(), val.second->tails_);
    } else {
        tails_.push_back(it);
    }
}

double IndexKey::selectivity() {
    if (empty())
        assert(stats_.selectivity_ == 1.0);

    return stats_.selectivity_;
}

unsigned IndexKey::weight() {
    if (1/selectivity() > double(numeric_limits<unsigned>::max()))
        return numeric_limits<unsigned>::max() / 2;
    return 1/selectivity();
}

long IndexKey::nodes() {
    return stats_.nodes_;
}

int IndexKey::depth() {
    return stats_.depth_;
}

void IndexKey::collect_tails(list<IndexKey::iterator>& tails) {
    tails.splice(tails.end(), tails_);
}

void IndexKey::concat(shared_ptr<IndexKey> rhs) {
    assert(anchor & kAnchorRight);
    assert(rhs->anchor & kAnchorLeft);
    assert(!empty());

    if (rhs->empty())
        return;

    list<IndexKey::iterator> tails;
    collect_tails(tails);
    for (auto it = tails.begin(); it != tails.end(); ++it) {
        assert(!(*it)->second);
        (*it)->second = rhs;
    }
    if (anchor & kAnchorRepeat)
        anchor &= ~kAnchorLeft;
    if ((rhs->anchor & (kAnchorRepeat|kAnchorRight)) != kAnchorRight)
        anchor &= ~kAnchorRight;

    stats_ = stats_.concat(rhs->stats());
    rhs->collect_tails(tails_);
}

static string strprintf(const char *fmt, ...)
    __attribute__((format (printf, 1, 2)));

static string strprintf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    return string(buf);
}

static string StrChar(uchar c) {
    if (c > ' ' && c <= '~')
        return strprintf("%c", c);
    return strprintf("\\x%02x", c);
}

static string ToString(IndexKey *k, int indent = 0) {
    string out;
    if (k == 0)
        return strprintf("%*.s[]\n", indent, "");

    for (IndexKey::iterator it = k->begin(); it != k->end(); ++it) {
        out += strprintf("%*.s[%s-%s] -> \n",
                         indent, "",
                         StrChar(it->first.first).c_str(),
                         StrChar(it->first.second).c_str());
        out += ToString(it->second.get(), indent + 1);
    }
    return out;
}

string IndexKey::ToString() {
    string out = ::ToString(this, 0);

    out += "|";
    if (anchor & kAnchorLeft)
        out += "<";
    if (anchor & kAnchorRepeat)
        out += "*";
    if (anchor & kAnchorRight)
        out += ">";
    return out;
}

class IndexWalker : public Regexp::Walker<shared_ptr<IndexKey> > {
public:
    IndexWalker() { }
    virtual shared_ptr<IndexKey>
    PostVisit(Regexp* re, shared_ptr<IndexKey> parent_arg,
              shared_ptr<IndexKey> pre_arg,
              shared_ptr<IndexKey> *child_args, int nchild_args);

    virtual shared_ptr<IndexKey>
    ShortVisit(Regexp* re,
               shared_ptr<IndexKey> parent_arg);

private:
    IndexWalker(const IndexWalker&);
    void operator=(const IndexWalker&);
};

namespace {
    typedef map<pair<shared_ptr<IndexKey>, shared_ptr<IndexKey> >,
                shared_ptr<IndexKey> > alternate_cache;

    shared_ptr<IndexKey> Alternate(alternate_cache&,
                                   shared_ptr<IndexKey>,
                                   shared_ptr<IndexKey>);

    string RuneToString(Rune r) {
        char buf[UTFmax];
        int n = runetochar(buf, &r);
        return string(buf, n);
    }

    shared_ptr<IndexKey> Any() {
        return shared_ptr<IndexKey>(new IndexKey());
    }

    shared_ptr<IndexKey> Empty() {
        return shared_ptr<IndexKey>(new IndexKey(kAnchorBoth));
    }

    shared_ptr<IndexKey> Literal(string s) {
        shared_ptr<IndexKey> k = 0;
        for (string::reverse_iterator it = s.rbegin();
             it != s.rend(); ++it) {
            k = shared_ptr<IndexKey>(new IndexKey(pair<uchar, uchar>(*it, *it), k));
        }
        k->anchor = kAnchorBoth;
        return k;
    }

    shared_ptr<IndexKey> Literal(Rune r) {
        return Literal(RuneToString(r));
    }

    shared_ptr<IndexKey> Literal(Rune *runes, int nrunes) {
        string lit;

        for (int i = 0; i < nrunes; i++) {
            lit.append(RuneToString(runes[i]));
        }

        return Literal(lit);
    }

    shared_ptr<IndexKey> LexRange(const string &lo, const string& hi) {
        shared_ptr<IndexKey> out(new IndexKey(kAnchorBoth));

        if (lo.size() == 0 && hi.size() == 0)
            return out;
        if (lo.size() == 0)
            return Literal(hi);
        assert(hi.size() != 0);
        if (lo[0] < hi[0])
            out->insert(IndexKey::value_type
                        (pair<uchar, uchar>(lo[0], hi[0] - 1), 0));
        out->insert(IndexKey::value_type
                    (pair<uchar, uchar>(hi[0], hi[0]),
                     LexRange(lo.substr(1), hi.substr(1))));
        return out;
    }

    shared_ptr<IndexKey> CClass(CharClass *cc) {
        if (cc->size() > kMaxWidth)
            return Any();

        shared_ptr<IndexKey> k(new IndexKey(kAnchorBoth));

        for (CharClass::iterator i = cc->begin(); i != cc->end(); ++i) {
            if (i->lo < Runeself && i->lo < Runeself)
                k->insert(IndexKey::value_type
                          (pair<uchar, uchar>(i->lo, i->hi),
                           0));
            else {
                alternate_cache cache;
                k = Alternate(cache, k, LexRange(RuneToString(i->lo),
                                                 RuneToString(i->hi)));
            }
        }

        return k;
    }

    bool ShouldConcat(shared_ptr<IndexKey> lhs, shared_ptr<IndexKey> rhs) {
        assert(lhs && rhs);
        if (!(lhs->anchor & kAnchorRight) ||
            !(rhs->anchor & kAnchorLeft))
            return false;
        if (lhs->empty())
            return false;
        IndexKey::Stats concat = lhs->stats().concat(rhs->stats());
        if (concat.nodes_ >= kMaxNodes)
            return false;
        return true;
    }

    shared_ptr<IndexKey> Concat(shared_ptr<IndexKey> lhs, shared_ptr<IndexKey> rhs) {
        assert(lhs);
        shared_ptr<IndexKey> out = lhs;

        debug(2, "Concat([%s](%ld), [%s](%ld)) = ",
              lhs ? lhs->ToString().c_str() : "",
              lhs->nodes(),
              rhs ? rhs->ToString().c_str() : "",
              rhs->nodes());

        if (ShouldConcat(lhs, rhs)) {
            out->concat(rhs);
        } else  {
            out->anchor &= ~kAnchorRight;
        }

        debug(2, "[%s]\n", out->ToString().c_str());

        return out;
    }

    IndexKey::Stats TryConcat(shared_ptr<IndexKey> *start,
                              shared_ptr<IndexKey> *end) {
        IndexKey::Stats st = (*start)->stats();
        debug(4, "TryConcat: Searching suffix of length %ld\n",
              end - start);
        if (!*start || !((*start)->anchor & kAnchorRight) || (*start)->empty()) {
            debug(4, "!ConcatRight, returning early.\n");
            return st;
        }
        for (shared_ptr<IndexKey> *ptr = start + 1; ptr != end; ptr++) {
            if (!*(ptr) || !((*ptr)->anchor & kAnchorLeft))
                break;

            st = st.concat((*ptr)->stats());

            if (st.nodes_ >= kMaxNodes)
                break;
            if (((*ptr)->anchor & (kAnchorRepeat|kAnchorRight)) != kAnchorRight)
                break;
        }
        debug(4, "TryConcat: nodes=%ld, selectivity=%f\n",
              st.nodes_, st.selectivity_);
        return st;
    }

    bool Prefer(const IndexKey::Stats& lhs,
                const IndexKey::Stats& rhs) {
        return (lhs.selectivity_ < rhs.selectivity_);
        /*
        return (kRECost * lhs.selectivity_ + kNodeCost * lhs.nodes_ <
                kRECost * rhs.selectivity_ + kNodeCost * rhs.nodes_);
        */
    }

    shared_ptr<IndexKey> Concat(shared_ptr<IndexKey> *children,
                                int nchildren) {
        shared_ptr<IndexKey> *end = children + nchildren, *best_start = 0, *ptr;
        IndexKey::Stats best_stats;

        debug(3, "Concat: Searching %d positions\n", nchildren);
        for (ptr = children; ptr != end; ptr++) {
            IndexKey::Stats st = TryConcat(ptr, end);
            if (st.nodes_ > 1 && Prefer(st, best_stats)) {
                debug(3, "Concat: Found new best: %ld: %f\n",
                      ptr - children, st.selectivity_);
                best_start = ptr;
                best_stats = st;
            }
        }

        if (best_start == 0) {
            debug(3, "Concat: No good results found.\n");
            return Any();
        }

        shared_ptr<IndexKey> out = *best_start;
        for (ptr = best_start + 1; ptr != end; ptr++) {
            out = Concat(out, *ptr);
        }
        return out;
    }

    bool intersects(const pair<uchar, uchar>& left,
                    const pair<uchar, uchar>& right) {
        if (left.first <= right.first)
            return left.second >= right.first;
        else
            return right.second >= left.first;
    }

    enum {
        kTakeLeft  = 0x01,
        kTakeRight = 0x02,
        kTakeBoth  = 0x03
    };

    int Merge(alternate_cache& cache,
              shared_ptr<IndexKey> out,
              pair<uchar, uchar>& left,
              shared_ptr<IndexKey> lnext,
              pair<uchar, uchar>& right,
              shared_ptr<IndexKey> rnext) {
        if (intersects(left, right)) {
            debug(3, "Processing intersection: <%hhx,%hhx> vs. <%hhx,%hhx>\n",
                  left.first, left.second, right.first, right.second);
            if (left.first < right.first) {
                out->insert
                    (make_pair(make_pair(left.first,
                                         right.first - 1),
                               lnext));
                left.first = right.first;
            } else if (right.first < left.first) {
                out->insert
                    (make_pair(make_pair(right.first,
                                         left.first - 1),
                               rnext));
                right.first = left.first;
            }
            /* left and right now start at the same location */
            assert(left.first == right.first);

            uchar end = min(left.second, right.second);
            out->insert
                (make_pair(make_pair(left.first, end),
                           Alternate(cache, lnext, rnext)));
            if (left.second > end) {
                left.first = end+1;
                return kTakeRight;
            } else if (right.second > end) {
                right.first = end+1;
                return kTakeLeft;
            }
            return kTakeBoth;
        }
        /* Non-intersecting */
        if (left.first < right.first) {
            out->insert(make_pair(left, lnext));
            return kTakeLeft;
        }
        assert(right.first < left.first);
        out->insert(make_pair(right, rnext));
        return kTakeRight;
    }

    shared_ptr<IndexKey> AlternateInternal(alternate_cache& cache,
                                           shared_ptr<IndexKey> lhs,
                                           shared_ptr<IndexKey> rhs) {
        if (lhs == rhs)
            return lhs;
        if (lhs->empty() || rhs->empty() ||
            lhs->size() + rhs->size() >= kMaxWidth)
            return Any();

        static int recursion_depth = 0;
        RecursionCounter guard(recursion_depth);
        if (recursion_depth > kMaxRecursion)
            return Any();

        shared_ptr<IndexKey> out(new IndexKey
                                 ((lhs->anchor & rhs->anchor) |
                                  ((lhs->anchor | lhs->anchor) & kAnchorRepeat)));
        IndexKey::const_iterator lit, rit;
        lit = lhs->begin();
        rit = rhs->begin();
        pair<uchar, uchar> left;
        if (lit != lhs->end())
            left = lit->first;
        pair<uchar, uchar> right = rit->first;
        if (rit != rhs->end())
            right = rit->first;
        while (lit != lhs->end() && rit != rhs->end()) {
            int action = Merge(cache, out, left, lit->second, right, rit->second);
            if (action & kTakeLeft)
                if (++lit != lhs->end())
                    left = lit->first;
            if (action & kTakeRight)
                if (++rit != rhs->end())
                    right = rit->first;
        }

        if (lit != lhs->end()) {
            out->insert(make_pair(left, lit->second));
            ++lit;
        }
        if (rit != rhs->end()) {
            out->insert(make_pair(right, rit->second));
            ++rit;
        }

        for (; lit != lhs->end(); ++lit)
            out->insert(*lit);
        for (; rit != rhs->end(); ++rit)
            out->insert(*rit);

        return out;
    }

    shared_ptr<IndexKey> Alternate(alternate_cache& cache,
                                   shared_ptr<IndexKey> lhs,
                                   shared_ptr<IndexKey> rhs) {
        auto it = cache.find(make_pair(lhs, rhs));
        if (it != cache.end())
            return it->second;
        shared_ptr<IndexKey> out = AlternateInternal(cache, lhs, rhs);
        cache[make_pair(lhs, rhs)] = out;
        return out;
    }

};

shared_ptr<IndexKey> indexRE(const re2::RE2 &re) {
    IndexWalker walk;

    Regexp *sre = re.Regexp()->Simplify();
    shared_ptr<IndexKey> key = walk.WalkExponential(sre, 0, 10000);
    sre->Decref();

    if (key && key->weight() < kMinWeight)
        key = 0;
    return key;
}

shared_ptr<IndexKey>
IndexWalker::PostVisit(Regexp* re, shared_ptr<IndexKey> parent_arg,
                       shared_ptr<IndexKey> pre_arg,
                       shared_ptr<IndexKey> *child_args,
                       int nchild_args) {
    shared_ptr<IndexKey> key;

    switch (re->op()) {
    case kRegexpNoMatch:
    case kRegexpEmptyMatch:      // anywhere
    case kRegexpBeginLine:       // at beginning of line
    case kRegexpEndLine:         // at end of line
    case kRegexpBeginText:       // at beginning of text
    case kRegexpEndText:         // at end of text
    case kRegexpWordBoundary:    // at word boundary
    case kRegexpNoWordBoundary:  // not at word boundary
        key = Empty();
        break;

    case kRegexpAnyChar:
    case kRegexpAnyByte:
        key = Any();
        break;

    case kRegexpLiteral:
        key = Literal(re->rune());
        break;

    case kRegexpCharClass:
        key = CClass(re->cc());
        break;

    case kRegexpLiteralString:
        key = Literal(re->runes(), re->nrunes());
        break;

    case kRegexpConcat:
        key = Concat(child_args, nchild_args);
        break;

    case kRegexpAlternate:
        {
            alternate_cache cache;
            key = child_args[0];
            for (int i = 1; i < nchild_args; i++)
                key = Alternate(cache, key, child_args[i]);
        }
        break;

    case kRegexpStar:
    case kRegexpQuest:
        key = Any();
        break;

    case kRegexpCapture:
        key = child_args[0];
        break;

    case kRegexpRepeat:
        if (re->min() == 0)
            return Any();
    case kRegexpPlus:
        key = child_args[0];
        if ((key->anchor & kAnchorBoth) == kAnchorBoth)
            key->anchor |= kAnchorRepeat;
        break;

    default:
        assert(false);
    }

    assert(key);

    debug(1, "* INDEX %s ==> ", re->ToString().c_str());
    if (key)
        debug(1, "[weight %d, nodes %ld, depth %d]\n",
              key->weight(), key->nodes(), key->depth());
    else
        debug(1, "nul\n");

    debug(2, "           ==> [%s]\n",
          key ? key->ToString().c_str() : "nul");

    if (FLAGS_debug_index && key)
        key->check_rep();

    return key;
}

shared_ptr<IndexKey>
IndexWalker::ShortVisit(Regexp* re, shared_ptr<IndexKey> parent_arg) {
    return Any();
}

void IndexKey::check_rep() {
    pair<uchar, uchar> last = make_pair('\0', '\0');
    for (iterator it = begin(); it != end(); ++it) {
        assert(!intersects(last, it->first));
        last = it->first;
    }
}
