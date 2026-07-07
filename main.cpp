#include "httplib.h"
#include "json.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <regex>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace httplib;

// ---------------------------------------------------------------------------
// Config: all knobs the settings tab can adjust. Guarded by a mutex since
// httplib serves requests from multiple threads.
// ---------------------------------------------------------------------------
struct BrunchConfig {
  int min_counterparts = 5;
  int max_counterparts = 30;
  int min_word_length = 10;
  int max_word_length = 50;
  // Seed for deterministic word generation. When non-empty, the same seed +
  // phonology settings will generate the *same* Brunch words for the *same*
  // English words on any machine -- like a Minecraft world seed -- without
  // ever sharing the brunch.db file itself. Empty string means "random",
  // preserving the original non-deterministic behavior.
  std::string seed = "";
  // Blended European phonology: Nordic (Swedish/Norwegian/Danish), German,
  // and French sound patterns combined into one pool.
  std::vector<std::string> onsets = {
      // single consonants (common across all)
      "b",  "d",  "f",  "g",  "h",  "j",  "k",  "l",  "m",  "n",
      "p",  "r",  "s",  "t",  "v",  "z",
      // Nordic clusters
      "bj", "fj", "gj", "hj", "kj", "sj", "sk", "sl", "sm", "sn",
      "sp", "st", "sv", "kv",
      // German-flavored clusters
      "ch", "sch", "kn", "pf", "zw", "schw", "schl", "schm", "schn", "gl", "gr",
      // French-flavored clusters
      "gn", "ph", "qu", "tr", "pr", "cl", "fl", "br", "cr", "vr"};
  std::vector<std::string> vowels = {
      // Nordic
      "a", "e", "i", "o", "u", "y", "aa", "ee", "ie",
      // German-flavored
      "au", "ei", "eu", "oo",
      // French-flavored
      "ou", "eau", "ai", "oi"};
  std::vector<std::string> codas = {
      // Nordic
      "d",  "f",  "g",  "k",  "l",  "m",  "n",  "p",  "r",  "s",  "t",
      "ng", "nd", "sk", "st", "ld", "rk", "rt",
      // German-flavored
      "ch", "cht", "sch", "tz", "pf", "nt",
      // French-flavored
      "gne", "que", "elle", "eux", "eur"};
};

std::mutex config_mutex;
BrunchConfig g_config;

// Guards the check-then-act sequence in getBrunchTranslation: without this,
// concurrent requests for the same new word can each see "no rows yet" and
// each insert their own full counterpart batch, producing wildly inflated
// counterpart counts for common words. See getBrunchTranslation for details.
std::mutex dictionary_write_mutex;

json configToJson() {
  std::lock_guard<std::mutex> lock(config_mutex);
  json j;
  j["min_counterparts"] = g_config.min_counterparts;
  j["max_counterparts"] = g_config.max_counterparts;
  j["min_word_length"] = g_config.min_word_length;
  j["max_word_length"] = g_config.max_word_length;
  j["seed"] = g_config.seed;
  j["onsets"] = g_config.onsets;
  j["vowels"] = g_config.vowels;
  j["codas"] = g_config.codas;
  return j;
}

// Validates + applies incoming settings. Returns an error string, or empty on success.
std::string applyConfigUpdate(const json &body) {
  BrunchConfig next;
  {
    std::lock_guard<std::mutex> lock(config_mutex);
    next = g_config;
  }

  auto getIntField = [&](const char *key, int &target) -> std::string {
    if (!body.contains(key)) return "";
    if (!body[key].is_number_integer()) return std::string("Field '") + key + "' must be an integer.";
    int v = body[key].get<int>();
    if (v < 1 || v > 500) return std::string("Field '") + key + "' must be between 1 and 500.";
    target = v;
    return "";
  };

  std::string err;
  if (!(err = getIntField("min_counterparts", next.min_counterparts)).empty()) return err;
  if (!(err = getIntField("max_counterparts", next.max_counterparts)).empty()) return err;
  if (!(err = getIntField("min_word_length", next.min_word_length)).empty()) return err;
  if (!(err = getIntField("max_word_length", next.max_word_length)).empty()) return err;

  if (next.min_counterparts > next.max_counterparts)
    return "min_counterparts cannot exceed max_counterparts.";
  if (next.min_word_length > next.max_word_length)
    return "min_word_length cannot exceed max_word_length.";
  if (next.min_word_length < 1)
    return "min_word_length must be at least 1.";

  if (body.contains("seed")) {
    if (!body["seed"].is_string()) return "Field 'seed' must be a string.";
    std::string s = body["seed"].get<std::string>();
    if (s.length() > 128) return "Field 'seed' must be 128 characters or fewer.";
    next.seed = s;
  }

  auto getStringList = [&](const char *key, std::vector<std::string> &target) -> std::string {
    if (!body.contains(key)) return "";
    if (!body[key].is_array()) return std::string("Field '") + key + "' must be an array of strings.";
    std::vector<std::string> items;
    for (const auto &el : body[key]) {
      if (!el.is_string()) return std::string("Field '") + key + "' must contain only strings.";
      std::string s = el.get<std::string>();
      if (s.empty()) continue; // skip blanks silently
      for (char c : s) {
        if (!std::isalpha(static_cast<unsigned char>(c)))
          return std::string("Field '") + key + "' entries must contain only letters (got '" + s + "').";
      }
      std::string lower = s;
      for (char &c : lower) c = std::tolower(static_cast<unsigned char>(c));
      items.push_back(lower);
    }
    if (items.empty()) return std::string("Field '") + key + "' cannot be empty.";
    target = items;
    return "";
  };

  if (!(err = getStringList("onsets", next.onsets)).empty()) return err;
  if (!(err = getStringList("vowels", next.vowels)).empty()) return err;
  if (!(err = getStringList("codas", next.codas)).empty()) return err;

  {
    std::lock_guard<std::mutex> lock(config_mutex);
    g_config = next;
  }
  return "";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string readFile(const std::string &filepath) {
  std::ifstream file(filepath);
  if (!file.is_open()) return "";
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// FNV-1a 64-bit hash, used to turn (seed, english_word, variant_index) into a
// deterministic 64-bit value we can feed into a seeded RNG. Not cryptographic
// -- just needs to be stable and well-distributed across platforms/compilers,
// which std::hash does NOT guarantee (its output can differ between runs and
// standard library implementations). FNV-1a gives us the same output on any
// machine for the same input string, which is the whole point of seeds here.
uint64_t fnv1a64(const std::string &s) {
  uint64_t hash = 1469598103934665603ULL; // FNV offset basis
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ULL; // FNV prime
  }
  return hash;
}

// Generates one Brunch word. If a seed is configured, `variant_seed_key` (e.g.
// "myseed::hello::3") is hashed to deterministically seed the RNG so the same
// seed + phonology settings produce the same word on any machine. If no seed
// is configured, falls back to true randomness (random_device) so existing
// non-deterministic behavior is preserved by default.
std::string generateBrunchWord(const std::string *variant_seed_key = nullptr) {
  BrunchConfig cfg;
  {
    std::lock_guard<std::mutex> lock(config_mutex);
    cfg = g_config;
  }

  std::mt19937_64 seeded_gen;
  std::mt19937 *gen_ptr = nullptr;
  static thread_local std::mt19937 random_gen(std::random_device{}());

  bool use_seeded = variant_seed_key != nullptr && !cfg.seed.empty();
  if (use_seeded) {
    seeded_gen.seed(fnv1a64(cfg.seed + "::" + *variant_seed_key));
  } else {
    gen_ptr = &random_gen;
  }

  // Small shim so the rest of the function can call a single generator
  // interface regardless of which engine is active.
  auto nextInt = [&](int lo, int hi) -> int {
    if (use_seeded) {
      std::uniform_int_distribution<> dist(lo, hi);
      return dist(seeded_gen);
    } else {
      std::uniform_int_distribution<> dist(lo, hi);
      return dist(*gen_ptr);
    }
  };

  int lo = std::min(cfg.min_word_length, cfg.max_word_length);
  int hi = std::max(cfg.min_word_length, cfg.max_word_length);
  int target_length = nextInt(lo, hi);

  std::string result;

  auto buildFallback = [](const std::vector<std::string> &options) {
    std::string chars;
    for (const auto &s : options)
      for (char c : s)
        if (chars.find(c) == std::string::npos) chars += c;
    if (chars.empty()) chars = "a"; // ultimate fallback, should not happen
    return chars;
  };

  std::string onset_fallback = buildFallback(cfg.onsets);
  std::string vowel_fallback = buildFallback(cfg.vowels);
  std::string coda_fallback = buildFallback(cfg.codas);

  auto addPart = [&](const std::vector<std::string> &options,
                      const std::string &fallback_chars) {
    if ((int)result.length() >= target_length) return;
    int idx = nextInt(0, (int)options.size() - 1);
    const std::string &part = options[idx];

    if ((int)(result.length() + part.length()) <= target_length) {
      result += part;
    } else {
      int char_idx = nextInt(0, (int)fallback_chars.length() - 1);
      result += fallback_chars[char_idx];
    }
  };

  int safety_iterations = 0;
  while ((int)result.length() < target_length && safety_iterations < target_length * 4 + 20) {
    addPart(cfg.onsets, onset_fallback);
    addPart(cfg.vowels, vowel_fallback);
    int chance = nextInt(0, 1);
    if (chance == 1) {
      addPart(cfg.codas, coda_fallback);
    }
    safety_iterations++;
  }
  return result;
}

std::string applyCasing(const std::string &original, std::string target) {
  if (original.empty() || target.empty()) return target;

  bool has_alpha = false;
  bool all_upper = true;
  bool first_upper = std::isupper(static_cast<unsigned char>(original[0])) != 0;

  for (char c : original) {
    if (std::isalpha(static_cast<unsigned char>(c))) {
      has_alpha = true;
      if (!std::isupper(static_cast<unsigned char>(c))) all_upper = false;
    }
  }

  if (!has_alpha) return target;

  if (all_upper && original.length() > 1) {
    for (char &c : target) c = std::toupper(static_cast<unsigned char>(c));
  } else if (first_upper) {
    target[0] = std::toupper(static_cast<unsigned char>(target[0]));
  }
  return target;
}

// Forward Translation Engine: English -> Brunch
// Restored intentional behavior: each English word may have several Brunch
// counterparts (count controlled by min/max_counterparts), and every lookup
// -- including repeat lookups of a word already in the dictionary -- picks a
// fresh random counterpart. This is deliberately non-deterministic, UNLESS a
// seed is configured, in which case counterpart *generation* is deterministic
// (same words are generated on any machine) while the *pick* on each lookup
// is still randomized for the shoulder-surfing-resistant varying output.
std::string getBrunchTranslation(sqlite3 *db, const std::string &original_token) {
  std::string english_word = original_token;
  for (char &c : english_word) c = std::tolower(static_cast<unsigned char>(c));

  // Fixed: the read (SELECT existing counterparts) and the write (INSERT a
  // fresh batch if none exist) must be atomic together, or concurrent
  // requests for the same new word can each see zero rows and each insert
  // their own batch -- stacking up far beyond max_counterparts over many
  // overlapping requests. Locking here serializes translation of new words
  // across threads; lookups of already-known words still just read, so this
  // only adds contention the first time each distinct word is seen.
  std::lock_guard<std::mutex> write_lock(dictionary_write_mutex);

  sqlite3_stmt *stmt;
  std::string query = "SELECT brunch_word FROM dictionary WHERE english_word = ?;";
  sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, english_word.c_str(), -1, SQLITE_TRANSIENT);

  std::vector<std::string> counterparts;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    counterparts.push_back(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  std::string chosen_brunch;
  static thread_local std::mt19937 pick_gen(std::random_device{}());

  if (!counterparts.empty()) {
    std::uniform_int_distribution<> dist(0, (int)counterparts.size() - 1);
    chosen_brunch = counterparts[dist(pick_gen)];
  } else {
    std::string current_seed;
    int min_c, max_c;
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      min_c = g_config.min_counterparts;
      max_c = g_config.max_counterparts;
      current_seed = g_config.seed;
    }

    int counterparts_count;
    if (!current_seed.empty()) {
      // Deterministic count too, so the same seed always produces the same
      // *number* of counterparts for a given word, not just the same words.
      std::mt19937_64 count_gen(fnv1a64(current_seed + "::__count__::" + english_word));
      std::uniform_int_distribution<> count_dist(min_c, max_c);
      counterparts_count = count_dist(count_gen);
    } else {
      std::uniform_int_distribution<> count_dist(min_c, max_c);
      counterparts_count = count_dist(pick_gen);
    }

    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    std::string insert_query = "INSERT INTO dictionary (english_word, brunch_word) VALUES (?, ?);";
    sqlite3_stmt *insert_stmt;
    sqlite3_prepare_v2(db, insert_query.c_str(), -1, &insert_stmt, nullptr);

    std::vector<std::string> generated;
    generated.reserve(counterparts_count);
    for (int i = 0; i < counterparts_count; ++i) {
      std::string variant_key = english_word + "::" + std::to_string(i);
      std::string new_brunch_word = current_seed.empty()
          ? generateBrunchWord(nullptr)
          : generateBrunchWord(&variant_key);
      sqlite3_bind_text(insert_stmt, 1, english_word.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(insert_stmt, 2, new_brunch_word.c_str(), -1, SQLITE_TRANSIENT);
      // Best-effort insert; a UNIQUE collision on brunch_word (astronomically
      // rare in random mode, and possible-but-rare in seeded mode across
      // very different English words) just skips that one variant rather
      // than aborting the batch.
      if (sqlite3_step(insert_stmt) == SQLITE_DONE) {
        generated.push_back(new_brunch_word);
      }
      sqlite3_reset(insert_stmt);
    }
    sqlite3_finalize(insert_stmt);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);

    if (!generated.empty()) {
      std::uniform_int_distribution<> pick(0, (int)generated.size() - 1);
      chosen_brunch = generated[pick(pick_gen)];
    }
  }

  return applyCasing(original_token, chosen_brunch);
}

// Reverse Translation Engine: Brunch -> English
std::string getEnglishTranslation(sqlite3 *db, const std::string &original_token) {
  std::string brunch_word = original_token;
  for (char &c : brunch_word) c = std::tolower(static_cast<unsigned char>(c));

  sqlite3_stmt *stmt;
  std::string query = "SELECT english_word FROM dictionary WHERE brunch_word = ? LIMIT 1;";
  sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, brunch_word.c_str(), -1, SQLITE_TRANSIENT);

  std::string found_english;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    found_english = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);

  if (found_english.empty()) return original_token;
  return applyCasing(original_token, found_english);
}

std::vector<std::string> tokenize(const std::string &text) {
  std::vector<std::string> tokens;
  std::regex re(R"((\w+)|(\s+)|([^\w\s]))");
  auto words_begin = std::sregex_iterator(text.begin(), text.end(), re);
  auto words_end = std::sregex_iterator();
  for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
    tokens.push_back(i->str());
  }
  return tokens;
}

bool isWordToken(const std::string &token) {
  return !token.empty() && std::all_of(token.begin(), token.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_';
  });
}

bool isSpaceToken(const std::string &token) {
  return !token.empty() &&
         std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); });
}

// Result of a wordlist training run.
struct WordlistTrainResult {
  bool ok = false;
  std::string error; // set when ok == false
  long long total_words = 0;
  long long processed = 0;
};

// Runs training/words.txt through getBrunchTranslation once, word by word,
// invoking `on_progress(processed, total_words)` periodically (roughly every
// 0.5% of progress, at least every word for tiny lists) so callers can report
// status without duplicating the training loop. This exists so that two
// installs sharing the same seed (see BrunchConfig::seed) end up with the
// same dictionary entries for the same common words *without* ever
// exchanging brunch.db -- which is what makes reverse (Brunch -> English)
// translation actually work: without this, a fresh dictionary has no rows at
// all, so any Brunch text sent by someone else can't be looked up yet, seed
// or not.
//
// Used two ways:
//   1. Silently at startup, when the dictionary is empty (first run).
//   2. On demand via POST /dictionary/train-wordlist, e.g. after the user
//      deliberately changes the seed/phonology and wants to (re)generate a
//      shared vocabulary under the new settings.
WordlistTrainResult runWordlistTraining(sqlite3 *db,
                                         const std::function<void(long long, long long)> &on_progress) {
  WordlistTrainResult result;
  const std::string wordlist_path = "training/words.txt";
  std::string corpus = readFile(wordlist_path);
  if (corpus.empty()) {
    result.ok = false;
    result.error = "No training corpus found at '" + wordlist_path + "' (or it was empty).";
    return result;
  }

  auto tokens = tokenize(corpus);
  long long total_words = 0;
  for (const auto &t : tokens)
    if (isWordToken(t)) total_words++;

  if (total_words == 0) {
    result.ok = false;
    result.error = "'" + wordlist_path + "' contained no words.";
    return result;
  }

  result.total_words = total_words;
  long long processed = 0;
  long long report_every = std::max<long long>(1, total_words / 200); // ~200 updates max

  for (const auto &token : tokens) {
    if (!isWordToken(token)) continue;
    getBrunchTranslation(db, token); // side effect only; return value discarded
    processed++;
    if (on_progress && (processed % report_every == 0 || processed == total_words)) {
      on_progress(processed, total_words);
    }
  }

  result.processed = processed;
  result.ok = true;
  return result;
}

// Thin wrapper for the silent startup path: just logs to stderr.
void autoTrainFromWordlist(sqlite3 *db) {
  std::cerr << "[startup] First run detected (empty dictionary). Training from training/words.txt...\n";
  auto result = runWordlistTraining(db, [](long long processed, long long total) {
    std::cerr << "[startup] Trained " << processed << " / " << total << " words...\n";
  });
  if (!result.ok) {
    std::cerr << "[startup] " << result.error
              << " Skipping auto-training. Reverse translation of words shared "
                 "by someone else won't work until words are trained on both sides.\n";
    return;
  }
  std::cerr << "[startup] Auto-training complete (" << result.processed << " words processed).\n";
}

int main() {
  sqlite3 *db;
  if (sqlite3_open("brunch.db", &db)) {
    std::cerr << "Can't open database: " << sqlite3_errmsg(db) << "\n";
    return 1;
  }

  const char *sql_setup =
      "CREATE TABLE IF NOT EXISTS dictionary (english_word TEXT, brunch_word TEXT UNIQUE);"
      "CREATE INDEX IF NOT EXISTS idx_english_word ON dictionary(english_word);"
      "CREATE INDEX IF NOT EXISTS idx_brunch_word ON dictionary(brunch_word);";
  char *errmsg = nullptr;
  if (sqlite3_exec(db, sql_setup, nullptr, nullptr, &errmsg) != SQLITE_OK) {
    std::cerr << "DB setup error: " << (errmsg ? errmsg : "unknown") << "\n";
    sqlite3_free(errmsg);
  }

  // First-run auto-training: only kick in when the dictionary is genuinely
  // empty, so this never re-runs (or re-generates words) on every restart of
  // an already-used installation.
  {
    sqlite3_stmt *count_stmt;
    long long existing_rows = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM dictionary;", -1, &count_stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(count_stmt) == SQLITE_ROW) existing_rows = sqlite3_column_int64(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);

    if (existing_rows == 0) {
      autoTrainFromWordlist(db);
    }
  }

  Server svr;

  svr.Get("/", [](const Request &, Response &res) {
    std::string html = readFile("index.html");
    if (html.empty()) {
      res.status = 500;
      res.set_content("<h1>500 Internal Server Error</h1><p>Could not find index.html</p>", "text/html");
      return;
    }
    res.set_content(html, "text/html");
  });

  svr.Post("/translate", [db](const Request &req, Response &res) {
    try {
      auto json_req = json::parse(req.body);
      std::string input_text = json_req.at("text").get<std::string>();
      std::string mode = json_req.value("mode", "to_brunch");

      std::string translated_text;
      std::vector<std::string> tokens = tokenize(input_text);

      for (const std::string &token : tokens) {
        if (isSpaceToken(token)) {
          translated_text += token;
        } else if (isWordToken(token)) {
          translated_text += (mode == "to_english") ? getEnglishTranslation(db, token)
                                                      : getBrunchTranslation(db, token);
        } else {
          translated_text += token;
        }
      }

      json json_res;
      json_res["translated_text"] = translated_text;
      res.set_content(json_res.dump(), "application/json");

    } catch (const std::exception &e) {
      res.status = 400;
      json err;
      err["error"] = "Bad Request";
      res.set_content(err.dump(), "application/json");
    }
  });

  // Settings API: read current config
  svr.Get("/settings", [](const Request &, Response &res) {
    res.set_content(configToJson().dump(), "application/json");
  });

  // Settings API: update config (partial updates supported; only provided
  // fields are validated and applied)
  svr.Post("/settings", [](const Request &req, Response &res) {
    try {
      auto body = json::parse(req.body);
      std::string err = applyConfigUpdate(body);
      if (!err.empty()) {
        res.status = 422;
        json j;
        j["error"] = err;
        res.set_content(j.dump(), "application/json");
        return;
      }
      res.set_content(configToJson().dump(), "application/json");
    } catch (const std::exception &e) {
      res.status = 400;
      json j;
      j["error"] = "Invalid JSON body";
      res.set_content(j.dump(), "application/json");
    }
  });

  // Stats endpoint: aggregate dictionary metrics computed via SQL, plus
  // on-disk database size. Read-only; safe to poll from the frontend.
  svr.Get("/stats", [db](const Request &, Response &res) {
    json j;

    auto scalarInt = [&](const std::string &query) -> long long {
      sqlite3_stmt *stmt;
      long long value = 0;
      if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          value = sqlite3_column_int64(stmt, 0);
        }
      }
      sqlite3_finalize(stmt);
      return value;
    };

    auto scalarDouble = [&](const std::string &query) -> double {
      sqlite3_stmt *stmt;
      double value = 0.0;
      if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          value = sqlite3_column_double(stmt, 0);
        }
      }
      sqlite3_finalize(stmt);
      return value;
    };

    long long total_rows = scalarInt("SELECT COUNT(*) FROM dictionary;");
    long long distinct_english =
        scalarInt("SELECT COUNT(DISTINCT english_word) FROM dictionary;");
    long long distinct_brunch =
        scalarInt("SELECT COUNT(DISTINCT brunch_word) FROM dictionary;");
    double avg_brunch_len =
        scalarDouble("SELECT AVG(LENGTH(brunch_word)) FROM dictionary;");
    long long min_brunch_len =
        scalarInt("SELECT MIN(LENGTH(brunch_word)) FROM dictionary;");
    long long max_brunch_len =
        scalarInt("SELECT MAX(LENGTH(brunch_word)) FROM dictionary;");
    double avg_counterparts_per_word =
        distinct_english > 0 ? (double)total_rows / (double)distinct_english : 0.0;

    // Word with the most counterparts, for a fun/useful "most ambiguous word" stat
    sqlite3_stmt *top_stmt;
    std::string top_word;
    long long top_word_count = 0;
    std::string top_query =
        "SELECT english_word, COUNT(*) as c FROM dictionary "
        "GROUP BY english_word ORDER BY c DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, top_query.c_str(), -1, &top_stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(top_stmt) == SQLITE_ROW) {
        top_word = reinterpret_cast<const char *>(sqlite3_column_text(top_stmt, 0));
        top_word_count = sqlite3_column_int64(top_stmt, 1);
      }
    }
    sqlite3_finalize(top_stmt);

    // On-disk file size for brunch.db
    long long db_file_bytes = 0;
    {
      std::ifstream f("brunch.db", std::ios::binary | std::ios::ate);
      if (f.good()) db_file_bytes = static_cast<long long>(f.tellg());
    }

    j["total_rows"] = total_rows;
    j["distinct_english_words"] = distinct_english;
    j["distinct_brunch_words"] = distinct_brunch;
    j["avg_counterparts_per_word"] = avg_counterparts_per_word;
    j["avg_brunch_word_length"] = avg_brunch_len;
    j["min_brunch_word_length"] = min_brunch_len;
    j["max_brunch_word_length"] = max_brunch_len;
    j["most_ambiguous_word"] = top_word;
    j["most_ambiguous_word_counterparts"] = top_word_count;
    j["db_file_bytes"] = db_file_bytes;

    res.set_content(j.dump(), "application/json");
  });

  // Training endpoint: processes one batch of pre-tokenized text, translating
  // every word (English -> Brunch) purely to populate the dictionary. Output
  // is discarded -- the only side effect kept is new dictionary rows.
  //
  // Batch-based by design: the frontend now splits large corpora into
  // multiple sequential /train/batch requests (size configurable via the
  // Training tab's batch-size slider) rather than one giant streamed request.
  // This keeps each request small and fast, makes progress reporting trivial
  // (frontend already knows processed/total from how many batches it has
  // sent), and avoids holding one huge chunked connection open for the whole
  // corpus. Each call is a plain, synchronous JSON request/response.
  svr.Post("/train/batch", [db](const Request &req, Response &res) {
    json json_req;
    try {
      json_req = json::parse(req.body);
    } catch (const std::exception &) {
      res.status = 400;
      json err;
      err["error"] = "Invalid JSON body";
      res.set_content(err.dump(), "application/json");
      return;
    }

    if (!json_req.contains("text") || !json_req["text"].is_string()) {
      res.status = 400;
      json err;
      err["error"] = "Expected a 'text' string (one batch's worth of corpus text).";
      res.set_content(err.dump(), "application/json");
      return;
    }

    std::string text = json_req["text"].get<std::string>();
    auto tokens = tokenize(text);

    long long processed = 0;
    long long new_words_seen = 0;

    for (const auto &token : tokens) {
      if (!isWordToken(token)) continue;

      std::string lower = token;
      for (char &c : lower) c = std::tolower(static_cast<unsigned char>(c));

      bool already_known = false;
      {
        sqlite3_stmt *check_stmt;
        std::string q = "SELECT 1 FROM dictionary WHERE english_word = ? LIMIT 1;";
        sqlite3_prepare_v2(db, q.c_str(), -1, &check_stmt, nullptr);
        sqlite3_bind_text(check_stmt, 1, lower.c_str(), -1, SQLITE_TRANSIENT);
        already_known = (sqlite3_step(check_stmt) == SQLITE_ROW);
        sqlite3_finalize(check_stmt);
      }

      // We only care about the side effect (dictionary population), so the
      // returned translation is intentionally discarded.
      getBrunchTranslation(db, token);
      if (!already_known) new_words_seen++;
      processed++;
    }

    json j;
    j["processed"] = processed;
    j["new_words"] = new_words_seen;
    res.set_content(j.dump(), "application/json");
  });

  // Manual wordlist training endpoint: re-runs training/words.txt through the
  // engine on demand (e.g. after the user deliberately changes seed/phonology
  // settings and wants a shared vocabulary generated under them). Unlike the
  // silent startup path, this always runs when called, regardless of whether
  // the dictionary already has rows -- the frontend is responsible for
  // confirming with the user first. Streams NDJSON progress since a full
  // wordlist can take a little while.
  svr.Post("/dictionary/train-wordlist", [db](const Request &, Response &res) {
    res.set_header("Content-Type", "application/x-ndjson");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider(
        "application/x-ndjson",
        [db](size_t /*offset*/, DataSink &sink) -> bool {
          bool client_disconnected = false;

          auto result = runWordlistTraining(db, [&](long long processed, long long total) {
            if (client_disconnected) return;
            json evt;
            evt["event"] = "progress";
            evt["processed"] = processed;
            evt["total_words"] = total;
            std::string line = evt.dump() + "\n";
            if (!sink.write(line.data(), line.size())) client_disconnected = true;
          });

          json done_evt;
          if (!result.ok) {
            done_evt["event"] = "error";
            done_evt["error"] = result.error;
          } else {
            done_evt["event"] = "done";
            done_evt["processed"] = result.processed;
            done_evt["total_words"] = result.total_words;
          }
          std::string line = done_evt.dump() + "\n";
          sink.write(line.data(), line.size());
          sink.done();
          return true;
        });
  });

  // Danger-zone endpoint: wipe the learned dictionary so new phonology
  // settings take effect on fresh words (existing rows are untouched by
  // settings changes otherwise, since translations are only ever generated
  // once per English word).
  svr.Post("/dictionary/clear", [db](const Request &, Response &res) {
    char *errmsg = nullptr;
    if (sqlite3_exec(db, "DELETE FROM dictionary;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
      res.status = 500;
      json j;
      j["error"] = errmsg ? errmsg : "unknown error";
      sqlite3_free(errmsg);
      res.set_content(j.dump(), "application/json");
      return;
    }
    json j;
    j["status"] = "cleared";
    res.set_content(j.dump(), "application/json");
  });

  std::cout << "Bidirectional Brunch Server running at http://localhost:8080\n";
  svr.listen("0.0.0.0", 8080);

  sqlite3_close(db);
  return 0;
}
