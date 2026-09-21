#pragma once
#include <string>
#include <vector>

// Short context prepared locally before each call, to spend fewer tokens.
// No model and no service: plain lexical relevance (BM25) over the salon's own text.
namespace ShortContext
{
    // Lower-case search terms: accents folded (é -> e), French/English stop words and
    // words shorter than 3 letters dropped.
    std::vector<std::string> Terms(const std::string& text);

    // Indices of the documents relevant to the query, best first (at most maxCount;
    // documents sharing no term with the query are never returned).
    std::vector<size_t> Rank(const std::vector<std::string>& documents, const std::string& query, size_t maxCount);
}
