#include "ShortContext.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace ShortContext
{
    namespace
    {
        // Latin-1 supplement (U+00C0..U+00FF) folded to ASCII, lower case; 0 = separator.
        char FoldLatin1(unsigned code)
        {
            static const char* kMap =
                "aaaaaaaceeeeiiii" // C0-CF
                "dnooooo\0ouuuuyts" // D0-DF (× is a separator, ß -> s)
                "aaaaaaaceeeeiiii" // E0-EF
                "dnooooo\0ouuuuyty"; // F0-FF (÷ is a separator)
            return code >= 0xC0 && code <= 0xFF ? kMap[code - 0xC0] : 0;
        }

        const std::set<std::string>& StopWords()
        {
            static const std::set<std::string> words = {
                "les", "des", "une", "est", "sont", "pas", "que", "qui", "quoi", "dans", "pour", "par", "sur", "avec",
                "sans", "mais", "donc", "car", "ces", "cet", "cette", "son", "sa", "ses", "leur", "leurs", "nous", "vous",
                "ils", "elles", "elle", "lui", "moi", "toi", "mon", "ton", "mes", "tes", "aux", "ete", "etre", "avoir",
                "fait", "faire", "comme", "tout", "tous", "toute", "toutes", "plus", "moins", "tres", "bien", "aussi",
                "alors", "encore", "deja", "meme", "peu", "peut", "entre", "vers", "chez", "quand", "comment", "ou",
                "the", "and", "for", "are", "was", "with", "that", "this", "from", "have", "has", "not", "but", "you",
                "your", "its", "can", "will", "all", "any", "our", "they", "them", "then", "than", "into", "out"};
            return words;
        }
    }

    std::vector<std::string> Terms(const std::string& text)
    {
        std::vector<std::string> out;
        std::string word;
        auto flush = [&] {
            if (word.size() >= 3 && !StopWords().count(word))
                out.push_back(word);
            word.clear();
        };
        for (size_t i = 0; i < text.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 0x80)
            {
                if (std::isalnum(c))
                    word.push_back(static_cast<char>(std::tolower(c)));
                else
                    flush();
            }
            else if ((c & 0xE0) == 0xC0 && i + 1 < text.size())
            {
                const unsigned code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
                ++i;
                if (code == 0x153 || code == 0x152) // œ Œ
                    word += "oe";
                else if (const char folded = FoldLatin1(code))
                    word.push_back(folded);
                else
                    flush();
            }
            else
            {
                // Other scripts and symbols: skip the whole sequence, as a separator.
                while (i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80)
                    ++i;
                flush();
            }
        }
        flush();
        return out;
    }

    std::vector<size_t> Rank(const std::vector<std::string>& documents, const std::string& query, size_t maxCount)
    {
        const std::vector<std::string> queryTerms = Terms(query);
        const std::set<std::string> wanted(queryTerms.begin(), queryTerms.end());
        if (wanted.empty() || documents.empty() || maxCount == 0)
            return {};

        std::vector<std::map<std::string, int>> frequencies(documents.size());
        std::vector<size_t> lengths(documents.size());
        std::map<std::string, int> documentFrequency;
        double totalLength = 0;
        for (size_t d = 0; d < documents.size(); ++d)
        {
            const std::vector<std::string> terms = Terms(documents[d]);
            lengths[d] = terms.size();
            totalLength += static_cast<double>(terms.size());
            for (const std::string& t : terms)
                if (wanted.count(t) && frequencies[d][t]++ == 0)
                    ++documentFrequency[t];
        }
        const double averageLength = std::max(1.0, totalLength / static_cast<double>(documents.size()));
        const double n = static_cast<double>(documents.size());
        constexpr double k1 = 1.2, b = 0.75;

        std::vector<std::pair<double, size_t>> scored;
        for (size_t d = 0; d < documents.size(); ++d)
        {
            double score = 0;
            for (const auto& [term, tf] : frequencies[d])
            {
                const double df = documentFrequency[term];
                const double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
                const double f = tf;
                score += idf * f * (k1 + 1) / (f + k1 * (1 - b + b * static_cast<double>(lengths[d]) / averageLength));
            }
            if (score > 0)
                scored.push_back({score, d});
        }
        // Best first; on a tie the more recent (later) document wins.
        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b2) {
            return a.first != b2.first ? a.first > b2.first : a.second > b2.second;
        });
        std::vector<size_t> out;
        for (size_t i = 0; i < scored.size() && out.size() < maxCount; ++i)
            out.push_back(scored[i].second);
        return out;
    }
}
