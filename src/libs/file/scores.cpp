#include "scores.h"

#include <format>
#include <iterator>
#include <numeric>
#include <regex>

#include "../gfx/color_utils.h"
#include "../math/sha256.h"
#include "../network/network.h"
#include "../parsers/song_parser.h"

static void ensure_dan_table(sqlite3* db);

// Z-DON: a database first made before player ids became text still declares players.player_id as INTEGER PRIMARY
// KEY (the rowid) and scores.player_id as INTEGER. A server access code (20 digits, past the 64-bit range) can't be
// stored in the first, so that player had no row (no name on the nameplate, no settings), and turns into an
// inexact REAL in the second. Rebuilds such a table once from its own definition (columns added since are kept)
// with player_id TEXT, as the current schema has it; the whole database is copied first.
static bool zdon_column_type_is_integer(sqlite3* db, const char* table) {
    std::string type;
    sqlite3_stmt* stmt = nullptr;
    const std::string q = std::string("SELECT type FROM pragma_table_info('") + table + "') WHERE name = 'player_id';";
    if (sqlite3_prepare_v2(db, q.c_str(), -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    std::ranges::transform(type, type.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return type == "INTEGER";
}

static bool zdon_rebuild_with_text_id(sqlite3* db, const char* table, const std::regex& id_column,
                                      const char* replacement) {
    std::string create_sql;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?;", -1, &stmt,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            create_sql = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
    }
    sqlite3_finalize(stmt);
    const std::regex table_name(std::string(R"(^\s*CREATE\s+TABLE\s+"?)") + table + R"("?\s*\()", std::regex::icase);
    if (!std::regex_search(create_sql, id_column) || !std::regex_search(create_sql, table_name)) {
        spdlog::warn("ScoresManager: {}.player_id is INTEGER but its definition wasn't recognised; left as it is",
                     table);
        return false;
    }
    const std::string tmp = std::string(table) + "_zdon_text";
    std::string new_sql =
        std::regex_replace(create_sql, id_column, replacement, std::regex_constants::format_first_only);
    new_sql =
        std::regex_replace(new_sql, table_name, "CREATE TABLE " + tmp + "(", std::regex_constants::format_first_only);
    const std::string sql = "BEGIN; DROP TABLE IF EXISTS " + tmp + "; " + new_sql + "; INSERT INTO " + tmp +
                            " SELECT * FROM " + table + "; DROP TABLE " + table + "; ALTER TABLE " + tmp +
                            " RENAME TO " + table + "; COMMIT;";
    char* err = nullptr;
    const bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
    if (ok) {
        spdlog::info("ScoresManager: {}.player_id is now TEXT", table);
    } else {
        spdlog::error("ScoresManager: fixing {}.player_id failed: {}", table, err ? err : "?");
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    sqlite3_free(err);
    return ok;
}

static void zdon_players_id_as_text(sqlite3* db, const fs::path& db_path) {
    const bool players = zdon_column_type_is_integer(db, "players");
    const bool scores = zdon_column_type_is_integer(db, "scores");
    if (!players && !scores) {
        return;
    }
    const std::string backup = db_path.string() + ".bak-before-text-ids";
    std::error_code ec;
    if (!fs::exists(backup, ec)) {
        const std::string vacuum = "VACUUM INTO '" + backup + "';";
        if (sqlite3_exec(db, vacuum.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
            spdlog::error("ScoresManager: could not back up {} before fixing player_id: {}", db_path.string(),
                          sqlite3_errmsg(db));
            return;
        }
        spdlog::info("ScoresManager: copied the database to {} before fixing player_id", backup);
    }
    int foreign_keys = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA foreign_keys;", -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        foreign_keys = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "PRAGMA foreign_keys = OFF;", nullptr, nullptr, nullptr);
    if (players) {
        static const std::regex pk(R"(player_id\s+INTEGER\s+PRIMARY\s+KEY(\s+AUTOINCREMENT)?)", std::regex::icase);
        zdon_rebuild_with_text_id(db, "players", pk, "player_id TEXT PRIMARY KEY");
    }
    if (scores) {
        static const std::regex col(R"(player_id\s+INTEGER\b)", std::regex::icase);
        zdon_rebuild_with_text_id(db, "scores", col, "player_id TEXT");
    }
    if (foreign_keys) {
        sqlite3_exec(db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    }
}

ScoresManager::ScoresManager(const fs::path& db_path) {
    if (sqlite3_open(db_path.string().c_str(), &db_fsd) != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_fsd);
        sqlite3_close(db_fsd);
        db_fsd = nullptr;
        throw std::runtime_error("Failed to open database: " + err);
    }

    sqlite3_stmt* version_stmt = nullptr;
    if (sqlite3_prepare_v2(db_fsd, "PRAGMA user_version;", -1, &version_stmt, nullptr) != SQLITE_OK ||
        sqlite3_step(version_stmt) != SQLITE_ROW) {
        std::string err = sqlite3_errmsg(db_fsd);
        sqlite3_finalize(version_stmt);
        sqlite3_close(db_fsd);
        db_fsd = nullptr;
        throw std::runtime_error("Failed to read database schema version: " + err);
    }

    int version = sqlite3_column_int(version_stmt, 0);
    sqlite3_finalize(version_stmt);

    std::string create_players = "CREATE TABLE IF NOT EXISTS players"
                                 "(player_id TEXT PRIMARY KEY,"
                                 "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
                                 "username TEXT NOT NULL UNIQUE,"
                                 "title TEXT NOT NULL,"
                                 "title_bg INTEGER NOT NULL DEFAULT 0,"
                                 "dan INTEGER NOT NULL DEFAULT -1,"
                                 "gold BOOL NOT NULL DEFAULT 0,"
                                 "rainbow BOOL NOT NULL DEFAULT 0,"
                                 "modifier_auto BOOL NOT NULL DEFAULT 0,"
                                 "modifier_speed INTEGER NOT NULL DEFAULT 10,"
                                 "modifier_display BOOL NOT NULL DEFAULT 0,"
                                 "modifier_inverse BOOL NOT NULL DEFAULT 0,"
                                 "modifier_random INTEGER NOT NULL DEFAULT 0,"
                                 "neiro_index INTEGER NOT NULL DEFAULT 0,"
                                 "chara_color_1 TEXT NOT NULL DEFAULT '#68BFC0',"
                                 "chara_color_2 TEXT NOT NULL DEFAULT '#F94728',"
                                 "chara_color_3 TEXT NOT NULL DEFAULT '#F9F0E1',"
                                 "chara_head_index INTEGER NOT NULL DEFAULT 0,"
                                 "chara_body_index INTEGER NOT NULL DEFAULT 0,"
                                 "chara_cos_index INTEGER NOT NULL DEFAULT 0,"
                                 "chara_is_costume BOOL NOT NULL DEFAULT 1,"
                                 "chara_paint_index INTEGER NOT NULL DEFAULT 0,"
                                 "chara_face_index INTEGER NOT NULL DEFAULT 0,"
                                 "chara_acce_index INTEGER NOT NULL DEFAULT 0,"
                                 "modifier_skip BOOL NOT NULL DEFAULT 0);";

    std::string create_songs = "CREATE TABLE IF NOT EXISTS songs"
                               "(song_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               "hash_0 TEXT,"
                               "hash_1 TEXT,"
                               "hash_2 TEXT,"
                               "hash_3 TEXT,"
                               "hash_4 TEXT,"
                               "title TEXT NOT NULL,"
                               "subtitle TEXT,"
                               "UNIQUE(hash_0, hash_1, hash_2, hash_3, hash_4));";

    std::string create_scores = "CREATE TABLE IF NOT EXISTS scores"
                                "(score_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                "player_id TEXT NOT NULL REFERENCES players(player_id),"
                                "hash TEXT,"
                                "difficulty INTEGER NOT NULL,"
                                "crown INTEGER NOT NULL,"
                                "rank INTEGER NOT NULL,"
                                "score INTEGER NOT NULL,"
                                "good INTEGER NOT NULL,"
                                "ok INTEGER NOT NULL,"
                                "bad INTEGER NOT NULL,"
                                "drumroll INTEGER NOT NULL,"
                                "max_combo INTEGER NOT NULL,"
                                "played_at INTEGER NOT NULL DEFAULT 0,"
                                "modifiers TEXT NOT NULL DEFAULT '{}');";

    char* errmsg = nullptr;
    if (sqlite3_exec(db_fsd, create_players.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
        spdlog::error("Failed to create players table: {}", errmsg);
        sqlite3_free(errmsg);
    }
    if (sqlite3_exec(db_fsd, create_songs.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
        spdlog::error("Failed to create songs table: {}", errmsg);
        sqlite3_free(errmsg);
    }
    if (sqlite3_exec(db_fsd, create_scores.c_str(), nullptr, nullptr, &errmsg) != SQLITE_OK) {
        spdlog::error("Failed to create scores table: {}", errmsg);
        sqlite3_free(errmsg);
    }

    auto run_migration = [&](const char* sql) -> bool {
        char* mig_errmsg = nullptr;
        if (sqlite3_exec(db_fsd, sql, nullptr, nullptr, &mig_errmsg) == SQLITE_OK) {
            return true;
        }
        bool duplicate_column = mig_errmsg && std::string(mig_errmsg).contains("duplicate column");
        if (!duplicate_column) {
            spdlog::error("ScoresManager migration failed: {} ({})", mig_errmsg ? mig_errmsg : "?", sql);
        }
        sqlite3_free(mig_errmsg);
        return duplicate_column;
    };

    bool migrations_ok = true;
    if (version < 2) {
        const char* migrations[] = {
            "ALTER TABLE players ADD COLUMN modifier_auto BOOL NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN title TEXT NOT NULL DEFAULT '';",
            "ALTER TABLE players ADD COLUMN title_bg INTEGER NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN dan INTEGER NOT NULL DEFAULT -1;",
            "ALTER TABLE players ADD COLUMN gold BOOL NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN rainbow BOOL NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN modifier_speed INTEGER NOT NULL DEFAULT 10;",
            "ALTER TABLE players ADD COLUMN modifier_display BOOL NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN modifier_inverse BOOL NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN modifier_random INTEGER NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN neiro_index INTEGER NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN chara_color_1 TEXT NOT NULL DEFAULT '#68BFC0';",
            "ALTER TABLE players ADD COLUMN chara_color_2 TEXT NOT NULL DEFAULT '#F94728';",
            "ALTER TABLE players ADD COLUMN chara_color_3 TEXT NOT NULL DEFAULT '#F9F0E1';",
            "ALTER TABLE players ADD COLUMN chara_head_index INTEGER NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN chara_body_index INTEGER NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN chara_cos_index INTEGER NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN chara_is_costume BOOL NOT NULL DEFAULT 1;",
            "ALTER TABLE players ADD COLUMN chara_paint_index INTEGER NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN chara_face_index INTEGER NOT NULL DEFAULT 0;",
            "ALTER TABLE players ADD COLUMN chara_acce_index INTEGER NOT NULL DEFAULT 0;",
        };
        for (const char* sql : migrations) {
            if (!run_migration(sql)) {
                migrations_ok = false;
            }
        }
    }
    run_migration("ALTER TABLE scores ADD COLUMN played_at INTEGER NOT NULL DEFAULT 0;");

    if (version < 4 && !run_migration("ALTER TABLE scores ADD COLUMN modifiers TEXT NOT NULL DEFAULT '{}';")) {
        migrations_ok = false;
    }

    // Migration v5: Convert integer player_ids to string access codes
    if (version < 5) {
        spdlog::info("Running migration v5: Converting integer player_ids to string access codes");

        // First, check if there are integer player_ids that need conversion
        // Use GLOB for pattern matching (SQLite doesn't support [^...] in LIKE)
        // Also exclude very long numeric IDs (long numeric access codes from server)
        sqlite3_stmt* stmt;
        const char* check_query = "SELECT COUNT(*) FROM players WHERE player_id NOT GLOB '*[^0-9]*' AND player_id != "
                                  "'' AND player_id NOT IN ('0') AND LENGTH(player_id) <= 10;";
        int has_integer_ids = 0;
        if (sqlite3_prepare_v2(db_fsd, check_query, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                has_integer_ids = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }

        if (has_integer_ids > 0) {
            spdlog::info("Found {} integer player_ids to migrate", has_integer_ids);

            // Get all integer player_ids and their usernames
            // Only process IDs with length <= 10 (excludes long server access codes)
            const char* select_query = "SELECT player_id, username FROM players WHERE player_id NOT GLOB '*[^0-9]*' "
                                       "AND player_id != '' AND player_id NOT IN ('0') AND LENGTH(player_id) <= 10;";
            if (sqlite3_prepare_v2(db_fsd, select_query, -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* old_id_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const char* username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

                    // Use strtoll with overflow checking instead of atoi
                    long long old_id = 0;
                    if (old_id_text) {
                        char* endptr = nullptr;
                        old_id = std::strtoll(old_id_text, &endptr, 10);
                        // Check for overflow or invalid input
                        if (endptr == old_id_text || *endptr != '\0' || old_id > INT_MAX || old_id < 0) {
                            spdlog::warn("Skipping invalid/overflow player_id: {}", old_id_text);
                            continue;
                        }
                    }
                    std::string new_id;

                    // Map old integer IDs to new string access codes
                    if (old_id == 1) {
                        new_id = "0";
                    } else if (old_id == 2) {
                        new_id = "1";
                    } else {
                        new_id = std::to_string(old_id);
                    }

                    spdlog::info("Migrating player {} (username: {}) to access code {}", old_id,
                                 username ? username : "unknown", new_id);

                    // Check if new_id already exists
                    sqlite3_stmt* check_stmt;
                    const char* check_exist = "SELECT 1 FROM players WHERE player_id = ?;";
                    if (sqlite3_prepare_v2(db_fsd, check_exist, -1, &check_stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(check_stmt, 1, new_id.c_str(), -1, SQLITE_TRANSIENT);
                        if (sqlite3_step(check_stmt) != SQLITE_ROW) {

                            // Update players table - use old_id_text for WHERE clause
                            const char* update_player = "UPDATE players SET player_id = ? WHERE player_id = ?;";
                            sqlite3_stmt* update_stmt;
                            if (sqlite3_prepare_v2(db_fsd, update_player, -1, &update_stmt, nullptr) == SQLITE_OK) {
                                sqlite3_bind_text(update_stmt, 1, new_id.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_text(update_stmt, 2, old_id_text, -1, SQLITE_TRANSIENT);
                                sqlite3_step(update_stmt);
                                sqlite3_finalize(update_stmt);
                            }

                            // Update scores table
                            const char* update_scores = "UPDATE scores SET player_id = ? WHERE player_id = ?;";
                            if (sqlite3_prepare_v2(db_fsd, update_scores, -1, &update_stmt, nullptr) == SQLITE_OK) {
                                sqlite3_bind_text(update_stmt, 1, new_id.c_str(), -1, SQLITE_TRANSIENT);
                                sqlite3_bind_text(update_stmt, 2, old_id_text, -1, SQLITE_TRANSIENT);
                                sqlite3_step(update_stmt);
                                sqlite3_finalize(update_stmt);
                            }
                        }
                        sqlite3_finalize(check_stmt);
                    }
                }
                sqlite3_finalize(stmt);
            }

            // Also ensure "0" and "1" exist for default players if they don't
            const char* ensure_defaults =
                "INSERT OR IGNORE INTO players (player_id, username, title) VALUES ('0', 'Don-chan', 'Donder Debut!');"
                "INSERT OR IGNORE INTO players (player_id, username, title) VALUES ('1', 'Kat-chan', 'Donder Debut!');"
                "UPDATE players SET title_bg = 2, chara_color_1 = '#F94728', chara_color_2 = '#68BFC0' WHERE player_id "
                "= '1' AND username = 'Kat-chan';";
            run_migration(ensure_defaults);
        }

        if (migrations_ok) {
            sqlite3_exec(db_fsd, "PRAGMA user_version = 5;", nullptr, nullptr, nullptr);
        } else {
            spdlog::error("ScoresManager: migration v5 failed, not raising user_version");
        }
    }

    // Migration v6: access codes went from 24 to 20 digits (server dropped the 4 leading digits).
    if (version < 6) {
        bool ok = run_migration("BEGIN;"
                                "UPDATE OR IGNORE players SET player_id = substr(player_id, 5) WHERE LENGTH(player_id) "
                                "= 24 AND player_id NOT GLOB '*[^0-9]*';"
                                "UPDATE scores SET player_id = substr(player_id, 5) WHERE LENGTH(player_id) = 24 AND "
                                "player_id NOT GLOB '*[^0-9]*';"
                                "COMMIT;");
        if (ok) {
            sqlite3_exec(db_fsd, "PRAGMA user_version = 6;", nullptr, nullptr, nullptr);
        } else {
            sqlite3_exec(db_fsd, "ROLLBACK;", nullptr, nullptr, nullptr);
            spdlog::error("ScoresManager: migration v6 failed, not raising user_version");
        }
    }

    run_migration("ALTER TABLE players ADD COLUMN modifier_skip BOOL NOT NULL DEFAULT 0;");
    run_migration("ALTER TABLE players ADD COLUMN practice_help_seen BOOL NOT NULL DEFAULT 0;");
    run_migration("ALTER TABLE players ADD COLUMN reward_info_seen BOOL NOT NULL DEFAULT 0;");
    run_migration("ALTER TABLE players ADD COLUMN ai_intro_seen BOOL NOT NULL DEFAULT 0;");

    // Migration v7: dan_results (one best-of row per course title) replaced by dan_scores (attempt history).
    if (version < 7 && run_migration("DROP TABLE IF EXISTS dan_results;")) {
        sqlite3_exec(db_fsd, "PRAGMA user_version = 7;", nullptr, nullptr, nullptr);
    }

    // Migration v8: dan costumes moved to the end of the costume list.
    if (version < 8 && run_migration("UPDATE players SET chara_cos_index = CASE "
                                     "WHEN chara_cos_index BETWEEN 36 AND 61 THEN chara_cos_index + 109 "
                                     "WHEN chara_cos_index BETWEEN 62 AND 170 THEN chara_cos_index - 26 "
                                     "ELSE chara_cos_index END;")) {
        sqlite3_exec(db_fsd, "PRAGMA user_version = 8;", nullptr, nullptr, nullptr);
    }

    ensure_dan_table(db_fsd);

    zdon_players_id_as_text(db_fsd, db_path);

    // Default player "1" (2P placeholder): Kat-chan, 2P chara colors, 2P title color. Only touches the untouched
    // default row.
    sqlite3_exec(
        db_fsd,
        "UPDATE players SET username = 'Kat-chan', title_bg = 1, chara_color_1 = '#F94728', chara_color_2 = '#68BFC0' "
        "WHERE player_id = '1' AND username = 'Player 2' AND NOT EXISTS (SELECT 1 FROM players WHERE username = "
        "'Kat-chan');",
        nullptr, nullptr, nullptr);

    load_score_cache();
}

ScoresManager::~ScoresManager() {
    if (add_song_stmt) {
        sqlite3_finalize(add_song_stmt);
    }
    if (get_box_record_stmt) {
        sqlite3_finalize(get_box_record_stmt);
    }
    if (store_box_record_stmt) {
        sqlite3_finalize(store_box_record_stmt);
    }
    if (store_song_cache_stmt) {
        sqlite3_finalize(store_song_cache_stmt);
    }
    if (remove_song_cache_stmt) {
        sqlite3_finalize(remove_song_cache_stmt);
    }
    if (db_fsd) {
        sqlite3_close(db_fsd);
    }
}

void ScoresManager::load_score_cache() {
    std::scoped_lock lock(maps_mutex);
    score_cache.clear();
    play_count_cache.clear();

    sqlite3_stmt* stmt;
    const char* query = "SELECT player_id, hash, difficulty, score, good, ok, bad, drumroll, max_combo, crown, rank "
                        "FROM scores "
                        "ORDER BY crown DESC, score DESC;";
    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("load_score_cache: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* player_id_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string player_id = player_id_text ? player_id_text : "";
        const unsigned char* hash_text = sqlite3_column_text(stmt, 1);
        std::string hash = hash_text ? reinterpret_cast<const char*>(hash_text) : "";
        int difficulty = sqlite3_column_int(stmt, 2);

        // Rows are ordered best-first, so the first row seen for a key is the best one.
        play_count_cache[{hash, player_id}]++;
        auto key = std::make_tuple(hash, difficulty, player_id);
        if (score_cache.contains(key)) {
            continue;
        }

        Score s;
        s.score = sqlite3_column_int(stmt, 3);
        s.good = sqlite3_column_int(stmt, 4);
        s.ok = sqlite3_column_int(stmt, 5);
        s.bad = sqlite3_column_int(stmt, 6);
        s.drumroll = sqlite3_column_int(stmt, 7);
        s.max_combo = sqlite3_column_int(stmt, 8);
        s.crown = static_cast<Crown>(sqlite3_column_int(stmt, 9));
        s.rank = static_cast<Rank>(sqlite3_column_int(stmt, 10));
        score_cache.emplace(std::move(key), s);
    }
    sqlite3_finalize(stmt);
}

void ScoresManager::py_taiko_import(const fs::path& old_db_path) {
    // Build reverse lookup: (en_name, jp_name) -> new hashes
    using NameKey = std::pair<std::string, std::string>;
    struct PairHash {
        size_t operator()(const NameKey& k) const {
            return std::hash<std::string>{}(k.first) ^ (std::hash<std::string>{}(k.second) << 1);
        }
    };
    std::unordered_map<NameKey, std::array<std::string, 5>, PairHash> name_to_hashes;

    std::unordered_map<fs::path, std::array<std::string, 5>> path_to_hashes_snapshot;
    {
        std::scoped_lock lock(maps_mutex);
        path_to_hashes_snapshot = path_to_hashes;
    }

    for (const auto& [path, hashes] : path_to_hashes_snapshot) {
        try {
            SongParser parser(path);
            std::string en = parser.metadata.title.contains("en") ? parser.metadata.title.at("en") : "";
            std::string ja = parser.metadata.title.contains("ja") ? parser.metadata.title.at("ja") : "";
            name_to_hashes[{en, ja}] = hashes;
        } catch (const std::exception& e) {
            spdlog::warn("py_taiko_import: failed to parse metadata for {}: {}", path.string(), e.what());
        }
    }

    spdlog::info("py_taiko_import: built name lookup with {} entries", name_to_hashes.size());

    // Open old DB
    sqlite3* old_db;
    if (sqlite3_open(old_db_path.string().c_str(), &old_db) != SQLITE_OK) {
        spdlog::error("py_taiko_import: failed to open old DB at {}: {}", old_db_path.string(), sqlite3_errmsg(old_db));
        sqlite3_close(old_db);
        return;
    }

    // Ensure the target player row exists (scores import into the current 1P profile).
    sqlite3_exec(db_fsd,
                 ("INSERT OR IGNORE INTO players (player_id, username, title) VALUES ('" + player_1 +
                  "', 'Don-chan', 'Donder Debut!');")
                     .c_str(),
                 nullptr, nullptr, nullptr);

    sqlite3_stmt* sel;
    const char* select_query = "SELECT en_name, jp_name, diff, score, good, ok, bad, drumroll, combo, clear "
                               "FROM Scores;";

    if (sqlite3_prepare_v2(old_db, select_query, -1, &sel, nullptr) != SQLITE_OK) {
        spdlog::error("py_taiko_import: failed to prepare SELECT: {}", sqlite3_errmsg(old_db));
        sqlite3_close(old_db);
        return;
    }

    int imported = 0, skipped = 0;

    sqlite3_exec(db_fsd, "BEGIN;", nullptr, nullptr, nullptr);

    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char* en_raw = reinterpret_cast<const char*>(sqlite3_column_text(sel, 0));
        const char* ja_raw = reinterpret_cast<const char*>(sqlite3_column_text(sel, 1));
        int diff = sqlite3_column_int(sel, 2);
        int score_val = sqlite3_column_int(sel, 3);
        int good = sqlite3_column_int(sel, 4);
        int ok = sqlite3_column_int(sel, 5);
        int bad = sqlite3_column_int(sel, 6);
        int drumroll = sqlite3_column_int(sel, 7);
        int combo = sqlite3_column_int(sel, 8);
        int crown_val = sqlite3_column_int(sel, 9);

        if (!en_raw || diff < 0 || diff > 4) {
            spdlog::warn("py_taiko_import: skipping row with null en_name or invalid diff {}", diff);
            skipped++;
            continue;
        }

        std::string en = en_raw;
        std::string ja = ja_raw ? ja_raw : "";

        auto it = name_to_hashes.find({en, ja});
        if (it == name_to_hashes.end()) {
            spdlog::warn("py_taiko_import: no hash match found for '{}' / '{}', skipping", en, ja);
            skipped++;
            continue;
        }

        const std::string& new_hash = it->second[diff];
        if (new_hash.empty()) {
            spdlog::warn("py_taiko_import: new hash empty for '{}' diff {}, skipping", en, diff);
            skipped++;
            continue;
        }

        // Duplicate check
        bool exists = false;
        int existing_crown = 0, existing_score = 0;
        {
            sqlite3_stmt* check_stmt;
            std::string check_query = "SELECT crown, score FROM scores WHERE player_id = '" + player_1 +
                                      "' AND hash = ? AND difficulty = ? "
                                      "ORDER BY crown DESC, score DESC LIMIT 1;";
            if (sqlite3_prepare_v2(db_fsd, check_query.c_str(), -1, &check_stmt, nullptr) != SQLITE_OK) {
                spdlog::warn("py_taiko_import: failed to prepare check statement, skipping");
                skipped++;
                continue;
            }
            sqlite3_bind_text(check_stmt, 1, new_hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(check_stmt, 2, diff);
            if (sqlite3_step(check_stmt) == SQLITE_ROW) {
                exists = true;
                existing_crown = sqlite3_column_int(check_stmt, 0);
                existing_score = sqlite3_column_int(check_stmt, 1);
            }
            sqlite3_finalize(check_stmt);
        }

        bool legacy_is_better =
            crown_val > existing_crown || (crown_val == existing_crown && score_val > existing_score);

        if (exists && !legacy_is_better) {
            spdlog::debug("py_taiko_import: existing score for '{}' diff {} is not worse, skipping", en, diff);
            skipped++;
            continue;
        }

        // Insert or update score
        {
            sqlite3_stmt* ins_stmt;
            std::string ins_query =
                exists
                    ? "UPDATE scores SET score = ?, good = ?, ok = ?, bad = ?, drumroll = ?, max_combo = ?, crown = ? "
                      "WHERE player_id = '" +
                          player_1 + "' AND hash = ? AND difficulty = ?;"
                    : "INSERT INTO scores "
                      "(player_id, hash, difficulty, score, good, ok, bad, drumroll, max_combo, crown, rank) "
                      "VALUES ('" +
                          player_1 + "', ?, ?, ?, ?, ?, ?, ?, ?, ?, 0);";
            if (sqlite3_prepare_v2(db_fsd, ins_query.c_str(), -1, &ins_stmt, nullptr) != SQLITE_OK) {
                spdlog::warn("py_taiko_import: failed to prepare {} statement, skipping", exists ? "update" : "insert");
                skipped++;
                continue;
            }
            if (exists) {
                sqlite3_bind_int(ins_stmt, 1, score_val);
                sqlite3_bind_int(ins_stmt, 2, good);
                sqlite3_bind_int(ins_stmt, 3, ok);
                sqlite3_bind_int(ins_stmt, 4, bad);
                sqlite3_bind_int(ins_stmt, 5, drumroll);
                sqlite3_bind_int(ins_stmt, 6, combo);
                sqlite3_bind_int(ins_stmt, 7, crown_val);
                sqlite3_bind_text(ins_stmt, 8, new_hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(ins_stmt, 9, diff);
            } else {
                sqlite3_bind_text(ins_stmt, 1, new_hash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(ins_stmt, 2, diff);
                sqlite3_bind_int(ins_stmt, 3, score_val);
                sqlite3_bind_int(ins_stmt, 4, good);
                sqlite3_bind_int(ins_stmt, 5, ok);
                sqlite3_bind_int(ins_stmt, 6, bad);
                sqlite3_bind_int(ins_stmt, 7, drumroll);
                sqlite3_bind_int(ins_stmt, 8, combo);
                sqlite3_bind_int(ins_stmt, 9, crown_val);
            }
            if (sqlite3_step(ins_stmt) == SQLITE_DONE) {
                imported++;
            } else {
                spdlog::warn("py_taiko_import: failed to {} score for '{}' diff {}: {}", exists ? "update" : "insert",
                             en, diff, sqlite3_errmsg(db_fsd));
                skipped++;
            }
            sqlite3_finalize(ins_stmt);
        }
    }

    sqlite3_exec(db_fsd, "COMMIT;", nullptr, nullptr, nullptr);

    sqlite3_finalize(sel);
    sqlite3_close(old_db);
    spdlog::info("py_taiko_import: done â€” {} imported, {} skipped", imported, skipped);

    if (imported > 0) {
        load_score_cache();
    }
}

void ScoresManager::export_to_hiroba(const std::string& access_code, const std::string& player_id) {
    sqlite3_stmt* stmt;
    const char* query =
        "SELECT hash, difficulty, crown, rank, score, good, ok, bad, drumroll, max_combo, played_at, modifiers "
        "FROM scores WHERE player_id = ? AND hash IS NOT NULL AND hash != '';";
    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("export_to_hiroba: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* hash_text = sqlite3_column_text(stmt, 0);
        std::string hash = hash_text ? reinterpret_cast<const char*>(hash_text) : "";
        if (hash.empty()) {
            continue;
        }

        Score s;
        int difficulty = sqlite3_column_int(stmt, 1);
        s.crown = static_cast<Crown>(sqlite3_column_int(stmt, 2));
        s.rank = static_cast<Rank>(sqlite3_column_int(stmt, 3));
        s.score = sqlite3_column_int(stmt, 4);
        s.good = sqlite3_column_int(stmt, 5);
        s.ok = sqlite3_column_int(stmt, 6);
        s.bad = sqlite3_column_int(stmt, 7);
        s.drumroll = sqlite3_column_int(stmt, 8);
        s.max_combo = sqlite3_column_int(stmt, 9);
        int64_t played_at = sqlite3_column_int64(stmt, 10);
        const unsigned char* modifiers_text = sqlite3_column_text(stmt, 11);
        std::string modifiers_json = modifiers_text ? reinterpret_cast<const char*>(modifiers_text) : "{}";

        InputLog input_log;
        network.submit_score(hash, difficulty, access_code, s, input_log, played_at, modifiers_json, false, -1);
        count++;
    }
    sqlite3_finalize(stmt);
    spdlog::info("export_to_hiroba: submitted {} scores", count);
}

int ScoresManager::apply_remote_scores(const std::vector<RemoteScore>& scores, const std::string& player_id) {
    int updated = 0;
    for (const RemoteScore& rs : scores) {
        auto local = get_score(rs.hash, rs.difficulty, player_id);
        bool remote_is_better = !local || rs.score.crown > local->crown ||
                                (rs.score.crown == local->crown && rs.score.score > local->score);
        if (remote_is_better) {
            save_score(rs.hash, rs.difficulty, player_id, rs.score, unix_now(), "{}");
            updated++;
        }
    }
    spdlog::info("sync_from_server: updated {} scores from hiroba", updated);
    return updated;
}

std::optional<Score> ScoresManager::get_score(const std::string& hash, int difficulty, const std::string& player_id) {
    std::scoped_lock lock(maps_mutex);
    auto it = score_cache.find(std::make_tuple(hash, difficulty, player_id));
    if (it != score_cache.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<PlayRecord> ScoresManager::recent_plays(const std::string& hash, int difficulty,
                                                    const std::string& player_id, int limit) {
    std::vector<PlayRecord> out;
    if (hash.empty() || limit <= 0) {
        return out;
    }
    sqlite3_stmt* stmt;
    const char* query = "SELECT score, crown, good, ok, bad, played_at FROM scores WHERE hash = ? AND difficulty = ? "
                        "AND player_id = ? ORDER BY played_at DESC, score_id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("recent_plays: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return out;
    }
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, difficulty);
    sqlite3_bind_text(stmt, 3, player_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(PlayRecord{sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1), sqlite3_column_int(stmt, 2),
                                 sqlite3_column_int(stmt, 3), sqlite3_column_int(stmt, 4),
                                 sqlite3_column_int64(stmt, 5)});
    }
    sqlite3_finalize(stmt);
    std::ranges::reverse(out);
    return out;
}

RecordCounts ScoresManager::get_record_counts(const std::string& player_id, int min_difficulty) {
    std::scoped_lock lock(maps_mutex);
    RecordCounts out;
    for (const auto& [key, s] : score_cache) {
        if (std::get<2>(key) != player_id || std::get<1>(key) < min_difficulty) {
            continue;
        }
        if (int r = static_cast<int>(s.rank); r >= 1 && r <= 7) {
            out.ranks[r - 1]++;
        }
        for (int c = 1; c <= std::min(static_cast<int>(s.crown), 3); c++) {
            out.crowns[c - 1]++;
        }
    }
    return out;
}

std::unordered_map<std::string, int> ScoresManager::get_play_counts(const std::string& player_id) {
    std::scoped_lock lock(maps_mutex);
    std::unordered_map<std::string, int> counts;
    for (const auto& [key, count] : play_count_cache) {
        if (key.second == player_id) {
            counts[key.first] += count;
        }
    }
    return counts;
}

Score ScoresManager::save_score(const std::string& hash, int difficulty, const std::string& player_id, Score score,
                                int64_t played_at, const std::string& modifiers_json, int64_t* out_score_id) {
    if (out_score_id) {
        *out_score_id = -1;
    }
    if (is_guest_id(player_id)) {
        return score;
    }
    sqlite3_stmt* stmt;

    const char* query =
        "INSERT INTO scores (player_id, hash, difficulty, score, good, ok, bad, drumroll, max_combo, crown, rank, "
        "played_at, modifiers) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("save_score: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return score;
    }

    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, nullptr);
    sqlite3_bind_int(stmt, 3, difficulty);
    sqlite3_bind_int(stmt, 4, score.score);
    sqlite3_bind_int(stmt, 5, score.good);
    sqlite3_bind_int(stmt, 6, score.ok);
    sqlite3_bind_int(stmt, 7, score.bad);
    sqlite3_bind_int(stmt, 8, score.drumroll);
    sqlite3_bind_int(stmt, 9, score.max_combo);
    sqlite3_bind_int(stmt, 10, static_cast<int>(score.crown));
    sqlite3_bind_int(stmt, 11, static_cast<int>(score.rank));
    sqlite3_bind_int64(stmt, 12, played_at);
    sqlite3_bind_text(stmt, 13, modifiers_json.c_str(), -1, nullptr);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("save_score: failed to insert score for hash {}: {}", hash, sqlite3_errmsg(db_fsd));
    } else if (out_score_id) {
        *out_score_id = sqlite3_last_insert_rowid(db_fsd);
    }
    sqlite3_finalize(stmt);
    spdlog::info("Saved score for hash: {} score: {} crown: {}", hash, score.score, static_cast<int>(score.crown));

    auto key = std::make_tuple(hash, difficulty, player_id);
    std::scoped_lock lock(maps_mutex);
    play_count_cache[{hash, player_id}]++;
    auto it = score_cache.find(key);
    bool is_better = it == score_cache.end() || score.crown > it->second.crown ||
                     (score.crown == it->second.crown && score.score > it->second.score);
    if (is_better) {
        if (it == score_cache.end()) {
            score_cache.emplace(key, score);
        } else {
            it->second = score;
        }
    }

    return score;
}

void ScoresManager::add_path_binding(const fs::path& path, const std::array<std::string, 5>& hashes) {
    std::scoped_lock lock(maps_mutex);
    path_to_hashes[path] = hashes;
    std::string single = std::accumulate(hashes.begin(), hashes.end(), std::string{});
    if (!single.empty()) {
        single_hash_to_path[single] = path;
    }
    for (const std::string& hash : hashes) {
        if (!hash.empty()) {
            diff_hash_to_path[hash] = path;
        }
    }
}

std::optional<fs::path> ScoresManager::get_path_by_hash(const std::string& single_hash) {
    if (single_hash.empty()) {
        return std::nullopt;
    }
    std::scoped_lock lock(maps_mutex);
    auto it = single_hash_to_path.find(single_hash);
    if (it != single_hash_to_path.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<fs::path> ScoresManager::get_path_by_diff_hash(const std::string& diff_hash) {
    std::scoped_lock lock(maps_mutex);
    auto it = diff_hash_to_path.find(diff_hash);
    if (it != diff_hash_to_path.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::array<std::string, 5> ScoresManager::get_hashes(const fs::path& path) {
    std::scoped_lock lock(maps_mutex);
    auto it = path_to_hashes.find(path);
    return it != path_to_hashes.end() ? it->second : std::array<std::string, 5>{};
}

std::string ScoresManager::get_single_hash(const fs::path& path) {
    std::scoped_lock lock(maps_mutex);
    auto it = path_to_hashes.find(path);
    if (it == path_to_hashes.end()) {
        return "";
    }
    return std::accumulate(it->second.begin(), it->second.end(), std::string{});
}

static void ensure_song_cache_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS song_cache ("
                 "path TEXT PRIMARY KEY, version INTEGER NOT NULL, mtime INTEGER NOT NULL, size INTEGER NOT NULL, "
                 "hash_0 TEXT, hash_1 TEXT, hash_2 TEXT, hash_3 TEXT, hash_4 TEXT, title TEXT, subtitle TEXT, "
                 "titles TEXT, subtitles TEXT, levels TEXT);",
                 nullptr, nullptr, nullptr);
    // tables made by version 1 lack the metadata columns (their rows are re-parsed anyway)
    static bool columns_checked = false;
    if (columns_checked) {
        return;
    }
    columns_checked = true;
    for (const char* col : {"titles", "subtitles", "levels"}) {
        std::string sql = std::string("ALTER TABLE song_cache ADD COLUMN ") + col + " TEXT;";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr); // fails harmlessly if present
    }
    // Z-DON: the write time from the directory listing (SongCacheEntry::ft_write); upstream ignores it
    sqlite3_exec(db, "ALTER TABLE song_cache ADD COLUMN ft_write INTEGER;", nullptr, nullptr, nullptr);
}

// language/text pairs as "lang \x1f text \x1e ..." (control characters no title uses)
static std::string encode_pairs(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::string out;
    for (const auto& [k, v] : pairs) {
        if (!out.empty()) {
            out += '\x1e';
        }
        out += k;
        out += '\x1f';
        out += v;
    }
    return out;
}

static std::vector<std::pair<std::string, std::string>> decode_pairs(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    if (s.empty()) {
        return out;
    }
    size_t start = 0;
    for (;;) {
        size_t end = s.find('\x1e', start);
        std::string item = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t sep = item.find('\x1f');
        if (sep != std::string::npos) {
            out.emplace_back(item.substr(0, sep), item.substr(sep + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

static std::string encode_levels(const std::array<int, 5>& lv) {
    std::string out;
    for (int i = 0; i < 5; i++) {
        out += (i ? "," : "") + std::to_string(lv[i]);
    }
    return out;
}

static std::array<int, 5> decode_levels(const std::string& s) {
    std::array<int, 5> lv = {-1, -1, -1, -1, -1};
    size_t start = 0;
    for (int i = 0; i < 5 && start <= s.size(); i++) {
        size_t end = s.find(',', start);
        try {
            lv[i] = std::stoi(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        } catch (...) {
            break;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return lv;
}

void ScoresManager::set_song_meta(const std::string& path, const SongCacheEntry& entry) {
    std::scoped_lock lock(song_meta_mutex);
    song_meta[path] = entry;
}

void ScoresManager::set_song_meta(const std::string& path, SongCacheEntry&& entry) {
    std::scoped_lock lock(song_meta_mutex);
    song_meta.insert_or_assign(path, std::move(entry));
}

std::optional<SongCacheEntry> ScoresManager::get_song_meta(const std::string& path) {
    std::scoped_lock lock(song_meta_mutex);
    auto it = song_meta.find(path);
    if (it == song_meta.end()) {
        return std::nullopt;
    }
    return it->second;
}

// Z-DON only (upstream does not know the table): per-player options of Z-DON's own (constant speed, ...)
static void ensure_option_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS zdon_player_options (player_id TEXT NOT NULL, key TEXT NOT NULL, "
                 "value INTEGER NOT NULL, PRIMARY KEY (player_id, key));",
                 nullptr, nullptr, nullptr);
}

int ScoresManager::get_zdon_option(const std::string& player_id, const std::string& key, int fallback) {
    ensure_option_table(db_fsd);
    int value = fallback;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd, "SELECT value FROM zdon_player_options WHERE player_id = ? AND key = ?;", -1, &stmt,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return value;
}

std::array<int, 10> ScoresManager::get_zdon_record_counts(const std::string& player_id, int min_difficulty) {
    std::array<int, 10> counts{};
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "SELECT MAX(rank), MAX(crown) FROM scores WHERE player_id = ? AND difficulty >= ? "
                           "GROUP BY hash, difficulty;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return counts;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, min_difficulty);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int rank = sqlite3_column_int(stmt, 0);
        const int crown = sqlite3_column_int(stmt, 1);
        if (rank >= 1 && rank <= 7) {
            counts[rank - 1]++;
        }
        if (crown >= 1 && crown <= 3) {
            counts[6 + crown]++;
        }
    }
    sqlite3_finalize(stmt);
    return counts;
}

static void ensure_ai_section_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS zdon_ai_sections (player_id TEXT NOT NULL, hash TEXT NOT NULL, "
                 "difficulty INTEGER NOT NULL, section INTEGER NOT NULL, crown INTEGER NOT NULL, "
                 "PRIMARY KEY (player_id, hash, difficulty, section));",
                 nullptr, nullptr, nullptr);
}

std::vector<int> ScoresManager::get_zdon_ai_section_crowns(const std::string& player_id, const std::string& hash,
                                                           int difficulty) {
    std::vector<int> crowns;
    ensure_ai_section_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "SELECT section, crown FROM zdon_ai_sections WHERE player_id = ? AND hash = ? AND "
                           "difficulty = ? ORDER BY section;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return crowns;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, difficulty);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int section = sqlite3_column_int(stmt, 0);
        if (section >= 0 && section < 16) {
            crowns.resize(std::max<size_t>(crowns.size(), static_cast<size_t>(section) + 1), 0);
            crowns[section] = sqlite3_column_int(stmt, 1);
        }
    }
    sqlite3_finalize(stmt);
    return crowns;
}

void ScoresManager::set_zdon_ai_section_crowns(const std::string& player_id, const std::string& hash, int difficulty,
                                               const std::vector<int>& crowns) {
    if (player_id.empty() || hash.empty()) {
        return;
    }
    ensure_ai_section_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "INSERT INTO zdon_ai_sections (player_id, hash, difficulty, section, crown) "
                           "VALUES (?, ?, ?, ?, ?) ON CONFLICT (player_id, hash, difficulty, section) "
                           "DO UPDATE SET crown = MAX(crown, excluded.crown);",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("set_zdon_ai_section_crowns: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    for (size_t i = 0; i < crowns.size(); ++i) {
        sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, difficulty);
        sqlite3_bind_int(stmt, 4, static_cast<int>(i));
        sqlite3_bind_int(stmt, 5, crowns[i]);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

static void ensure_ai_rank_tables(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS zdon_ai_rank (player_id TEXT PRIMARY KEY NOT NULL, points INTEGER NOT "
                 "NULL, rank INTEGER NOT NULL);"
                 "CREATE TABLE IF NOT EXISTS zdon_ai_first_wins (player_id TEXT NOT NULL, hash TEXT NOT NULL, "
                 "difficulty INTEGER NOT NULL, PRIMARY KEY (player_id, hash, difficulty));",
                 nullptr, nullptr, nullptr);
}

std::pair<int, int> ScoresManager::get_zdon_ai_rank(const std::string& player_id) {
    std::pair<int, int> out{0, 0};
    ensure_ai_rank_tables(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd, "SELECT points, rank FROM zdon_ai_rank WHERE player_id = ?;", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return out;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = {sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1)};
    }
    sqlite3_finalize(stmt);
    return out;
}

void ScoresManager::set_zdon_ai_rank(const std::string& player_id, int points, int rank) {
    if (player_id.empty()) {
        return;
    }
    ensure_ai_rank_tables(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "INSERT INTO zdon_ai_rank (player_id, points, rank) VALUES (?, ?, ?) ON CONFLICT "
                           "(player_id) DO UPDATE SET points = excluded.points, rank = excluded.rank;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, points);
    sqlite3_bind_int(stmt, 3, rank);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool ScoresManager::add_zdon_ai_first_win(const std::string& player_id, const std::string& hash, int difficulty) {
    if (player_id.empty() || hash.empty()) {
        return false;
    }
    ensure_ai_rank_tables(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "INSERT OR IGNORE INTO zdon_ai_first_wins (player_id, hash, difficulty) VALUES (?, ?, ?);",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, difficulty);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_changes(db_fsd) > 0;
}

static void ensure_ai_section_best_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS zdon_ai_section_best (player_id TEXT NOT NULL, hash TEXT NOT NULL, "
                 "difficulty INTEGER NOT NULL, section INTEGER NOT NULL, total INTEGER NOT NULL, score INTEGER NOT "
                 "NULL, good INTEGER NOT NULL, ok INTEGER NOT NULL, bad INTEGER NOT NULL, drumroll INTEGER NOT NULL, "
                 "PRIMARY KEY (player_id, hash, difficulty, section));",
                 nullptr, nullptr, nullptr);
}

std::vector<std::array<int, 5>> ScoresManager::get_zdon_ai_section_best(const std::string& player_id,
                                                                        const std::string& hash, int difficulty,
                                                                        int* total) {
    std::vector<std::array<int, 5>> best;
    if (total) {
        *total = 0;
    }
    ensure_ai_section_best_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "SELECT section, total, score, good, ok, bad, drumroll FROM zdon_ai_section_best WHERE "
                           "player_id = ? AND hash = ? AND difficulty = ? ORDER BY section;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return best;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, difficulty);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int section = sqlite3_column_int(stmt, 0);
        if (section >= 0 && section < 16) {
            best.resize(std::max<size_t>(best.size(), static_cast<size_t>(section) + 1), {0, 0, 0, 0, 0});
            for (int k = 0; k < 5; ++k) {
                best[section][k] = sqlite3_column_int(stmt, 2 + k);
            }
            if (total) {
                *total = sqlite3_column_int(stmt, 1);
            }
        }
    }
    sqlite3_finalize(stmt);
    return best;
}

bool ScoresManager::set_zdon_ai_section_best(const std::string& player_id, const std::string& hash, int difficulty,
                                             int total, const std::vector<std::array<int, 5>>& sections) {
    if (player_id.empty() || hash.empty()) {
        return false;
    }
    int old_total = 0;
    const auto old = get_zdon_ai_section_best(player_id, hash, difficulty, &old_total);
    if (!old.empty() && total <= old_total) {
        return false;
    }
    sqlite3_stmt* del;
    if (sqlite3_prepare_v2(db_fsd,
                           "DELETE FROM zdon_ai_section_best WHERE player_id = ? AND hash = ? AND difficulty = ?;", -1,
                           &del, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, player_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(del, 2, hash.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(del, 3, difficulty);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "INSERT INTO zdon_ai_section_best (player_id, hash, difficulty, section, total, score, "
                           "good, ok, bad, drumroll) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("set_zdon_ai_section_best: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return false;
    }
    for (size_t i = 0; i < sections.size(); ++i) {
        sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, difficulty);
        sqlite3_bind_int(stmt, 4, static_cast<int>(i));
        sqlite3_bind_int(stmt, 5, total);
        for (int k = 0; k < 5; ++k) {
            sqlite3_bind_int(stmt, 6 + k, sections[i][k]);
        }
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

void ScoresManager::set_zdon_option(const std::string& player_id, const std::string& key, int value) {
    if (player_id.empty()) {
        return;
    }
    ensure_option_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "INSERT OR REPLACE INTO zdon_player_options (player_id, key, value) VALUES (?, ?, ?);", -1,
                           &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("set_zdon_option: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, value);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Z-DON only: the PB pace points of each chart and player, "ms:score;ms:score;..."
static void ensure_ghost_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS zdon_ghost (hash TEXT NOT NULL, difficulty INTEGER NOT NULL, "
                 "player_id TEXT NOT NULL, data TEXT NOT NULL, PRIMARY KEY (hash, difficulty, player_id));",
                 nullptr, nullptr, nullptr);
}

std::vector<std::pair<float, int>> ScoresManager::get_zdon_ghost(const std::string& hash, int difficulty,
                                                                 const std::string& player_id) {
    std::vector<std::pair<float, int>> points;
    ensure_ghost_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd, "SELECT data FROM zdon_ghost WHERE hash = ? AND difficulty = ? AND player_id = ?;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return points;
    }
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, difficulty);
    sqlite3_bind_text(stmt, 3, player_id.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        for (const char* p = data; p && *p;) {
            char* end = nullptr;
            const float ms = std::strtof(p, &end);
            if (end == p || *end != ':') {
                break;
            }
            p = end + 1;
            const long v = std::strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            points.emplace_back(ms, static_cast<int>(v));
            p = *end == ';' ? end + 1 : end;
        }
    }
    sqlite3_finalize(stmt);
    return points;
}

void ScoresManager::set_zdon_ghost(const std::string& hash, int difficulty, const std::string& player_id,
                                   const std::vector<std::pair<float, int>>& points) {
    if (player_id.empty() || hash.empty() || points.empty()) {
        return;
    }
    ensure_ghost_table(db_fsd);
    std::string data;
    data.reserve(points.size() * 12);
    for (const auto& [ms, v] : points) {
        data += std::to_string(static_cast<long long>(std::lround(ms))) + ':' + std::to_string(v) + ';';
    }
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "INSERT OR REPLACE INTO zdon_ghost (hash, difficulty, player_id, data) VALUES (?, ?, ?, ?);",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("set_zdon_ghost: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, difficulty);
    sqlite3_bind_text(stmt, 3, player_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, data.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// Z-DON only (upstream does not know the table): 2D outfits, see get_outfit
static void ensure_outfit_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS zdon_outfits (player_id TEXT NOT NULL, skin TEXT NOT NULL, "
                 "outfit INTEGER NOT NULL, PRIMARY KEY (player_id, skin));",
                 nullptr, nullptr, nullptr);
}

int ScoresManager::get_outfit(const std::string& player_id, const std::string& skin) {
    const auto key = std::make_pair(player_id, skin);
    if (auto it = outfit_cache.find(key); it != outfit_cache.end()) {
        return it->second;
    }
    int outfit = -1; // no pick yet
    ensure_outfit_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd, "SELECT outfit FROM zdon_outfits WHERE player_id = ? AND skin = ?;", -1, &stmt,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, skin.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outfit = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    outfit_cache[key] = outfit;
    return outfit;
}

void ScoresManager::set_outfit(const std::string& player_id, const std::string& skin, int outfit) {
    ensure_outfit_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd, "INSERT OR REPLACE INTO zdon_outfits (player_id, skin, outfit) VALUES (?, ?, ?);",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("set_outfit: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, skin.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, outfit);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    outfit_cache[std::make_pair(player_id, skin)] = outfit;
    spdlog::info("outfit_save: player_id={} skin={} outfit={}", player_id, skin, outfit);
}

// Z-DON only (upstream does not know the table): dan song note counts, see DanNoteCount
static void ensure_dan_note_count_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS zdon_dan_note_counts ("
                 "path TEXT NOT NULL, difficulty INTEGER NOT NULL, version INTEGER NOT NULL, mtime INTEGER NOT NULL, "
                 "size INTEGER NOT NULL, notes INTEGER NOT NULL, PRIMARY KEY (path, difficulty));",
                 nullptr, nullptr, nullptr);
}

std::map<std::pair<std::string, int>, DanNoteCount> ScoresManager::load_dan_note_counts() {
    std::map<std::pair<std::string, int>, DanNoteCount> counts;
    ensure_dan_note_count_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd,
                           "SELECT path, difficulty, mtime, size, notes FROM zdon_dan_note_counts WHERE version = ?;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("load_dan_note_counts: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return counts;
    }
    sqlite3_bind_int(stmt, 1, SONG_CACHE_VERSION);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* path = sqlite3_column_text(stmt, 0);
        if (!path) {
            continue;
        }
        DanNoteCount c;
        c.mtime = sqlite3_column_int64(stmt, 2);
        c.size = sqlite3_column_int64(stmt, 3);
        c.notes = sqlite3_column_int(stmt, 4);
        counts.emplace(std::make_pair(std::string(reinterpret_cast<const char*>(path)), sqlite3_column_int(stmt, 1)),
                       c);
    }
    sqlite3_finalize(stmt);
    return counts;
}

void ScoresManager::store_dan_note_counts(
    const std::vector<std::pair<std::pair<std::string, int>, DanNoteCount>>& rows) {
    if (rows.empty()) {
        return;
    }
    ensure_dan_note_count_table(db_fsd);
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(
            db_fsd,
            "INSERT OR REPLACE INTO zdon_dan_note_counts (path, difficulty, version, mtime, size, notes) "
            "VALUES (?, ?, ?, ?, ?, ?);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("store_dan_note_counts: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    sqlite3_exec(db_fsd, "BEGIN;", nullptr, nullptr, nullptr);
    for (const auto& [key, c] : rows) {
        sqlite3_bind_text(stmt, 1, key.first.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, key.second);
        sqlite3_bind_int(stmt, 3, SONG_CACHE_VERSION);
        sqlite3_bind_int64(stmt, 4, c.mtime);
        sqlite3_bind_int64(stmt, 5, c.size);
        sqlite3_bind_int(stmt, 6, c.notes);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_exec(db_fsd, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_finalize(stmt);
}

// Z-DON only (upstream does not know the table): what a song select box shows of a chart, see
// get_song_box_records. `version` is SONG_CACHE_VERSION (the record carries its own format number).
static void ensure_box_record_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS zdon_song_box_records ("
                 "path TEXT PRIMARY KEY, version INTEGER NOT NULL, mtime INTEGER NOT NULL, size INTEGER NOT NULL, "
                 "ft_write INTEGER NOT NULL, record BLOB NOT NULL);",
                 nullptr, nullptr, nullptr);
}

std::vector<std::string> ScoresManager::get_song_box_records(const std::vector<std::string>& paths) {
    std::vector<std::string> out(paths.size());
    std::vector<std::optional<SongCacheEntry>> seen(paths.size());
    bool any = false;
    for (size_t i = 0; i < paths.size(); i++) {
        seen[i] = get_song_meta(paths[i]);
        any |= seen[i].has_value();
    }
    if (!any) {
        return out;
    }
    std::scoped_lock lock(box_record_mutex);
    if (!get_box_record_stmt) {
        ensure_box_record_table(db_fsd);
        if (sqlite3_prepare_v2(db_fsd,
                               "SELECT version, mtime, size, ft_write, record FROM zdon_song_box_records "
                               "WHERE path = ?;",
                               -1, &get_box_record_stmt, nullptr) != SQLITE_OK) {
            spdlog::error("get_song_box_records: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
            get_box_record_stmt = nullptr;
            return out;
        }
    }
    sqlite3_stmt* stmt = get_box_record_stmt;
    for (size_t i = 0; i < paths.size(); i++) {
        if (!seen[i]) {
            continue;
        }
        sqlite3_bind_text(stmt, 1, paths[i].c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == SONG_CACHE_VERSION &&
            sqlite3_column_int64(stmt, 1) == seen[i]->mtime && sqlite3_column_int64(stmt, 2) == seen[i]->size &&
            sqlite3_column_int64(stmt, 3) == seen[i]->ft_write) {
            const void* blob = sqlite3_column_blob(stmt, 4);
            const int n = sqlite3_column_bytes(stmt, 4);
            if (blob && n > 0) {
                out[i].assign(static_cast<const char*>(blob), static_cast<size_t>(n));
            }
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    return out;
}

void ScoresManager::store_song_box_records(const std::vector<std::pair<std::string, std::string>>& rows) {
    if (rows.empty()) {
        return;
    }
    std::scoped_lock lock(box_record_mutex);
    if (!store_box_record_stmt) {
        ensure_box_record_table(db_fsd);
        if (sqlite3_prepare_v2(db_fsd,
                               "INSERT OR REPLACE INTO zdon_song_box_records "
                               "(path, version, mtime, size, ft_write, record) VALUES (?, ?, ?, ?, ?, ?);",
                               -1, &store_box_record_stmt, nullptr) != SQLITE_OK) {
            spdlog::error("store_song_box_records: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
            store_box_record_stmt = nullptr;
            return;
        }
    }
    sqlite3_stmt* stmt = store_box_record_stmt;
    // a savepoint, not BEGIN: it nests in a transaction another thread may have open on this connection
    sqlite3_exec(db_fsd, "SAVEPOINT zdon_box_records;", nullptr, nullptr, nullptr);
    for (const auto& [path, record] : rows) {
        const std::optional<SongCacheEntry> seen = get_song_meta(path);
        if (!seen || record.empty()) {
            continue;
        }
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, SONG_CACHE_VERSION);
        sqlite3_bind_int64(stmt, 3, seen->mtime);
        sqlite3_bind_int64(stmt, 4, seen->size);
        sqlite3_bind_int64(stmt, 5, seen->ft_write);
        sqlite3_bind_blob(stmt, 6, record.data(), static_cast<int>(record.size()), SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_exec(db_fsd, "RELEASE zdon_box_records;", nullptr, nullptr, nullptr);
}

std::unordered_map<std::string, SongCacheEntry> ScoresManager::load_song_cache() {
    std::unordered_map<std::string, SongCacheEntry> cache;
    ensure_song_cache_table(db_fsd);
    sqlite3_stmt* stmt;
    const char* query = "SELECT path, mtime, size, hash_0, hash_1, hash_2, hash_3, hash_4, title, subtitle, "
                        "titles, subtitles, levels, ft_write FROM song_cache WHERE version = ?;";
    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("load_song_cache: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return cache;
    }
    sqlite3_bind_int(stmt, 1, SONG_CACHE_VERSION);
    auto text = [&](int col) {
        const unsigned char* t = sqlite3_column_text(stmt, col);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string{};
    };
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SongCacheEntry e;
        e.mtime = sqlite3_column_int64(stmt, 1);
        e.size = sqlite3_column_int64(stmt, 2);
        for (int i = 0; i < 5; i++) {
            e.hashes[i] = text(3 + i);
        }
        e.title = text(8);
        e.subtitle = text(9);
        e.titles = decode_pairs(text(10));
        e.subtitles = decode_pairs(text(11));
        e.levels = decode_levels(text(12));
        e.ft_write = sqlite3_column_type(stmt, 13) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 13);
        cache.emplace(text(0), std::move(e));
    }
    sqlite3_finalize(stmt);
    return cache;
}

void ScoresManager::store_song_cache(const std::string& path, const SongCacheEntry& entry) {
    if (!store_song_cache_stmt) {
        ensure_song_cache_table(db_fsd);
        const char* query =
            "INSERT OR REPLACE INTO song_cache "
            "(path, version, mtime, size, hash_0, hash_1, hash_2, hash_3, hash_4, title, subtitle, "
            "titles, subtitles, levels, ft_write) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_fsd, query, -1, &store_song_cache_stmt, nullptr) != SQLITE_OK) {
            spdlog::error("store_song_cache: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
            store_song_cache_stmt = nullptr;
            return;
        }
    }
    sqlite3_stmt* stmt = store_song_cache_stmt;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, SONG_CACHE_VERSION);
    sqlite3_bind_int64(stmt, 3, entry.mtime);
    sqlite3_bind_int64(stmt, 4, entry.size);
    for (int i = 0; i < 5; i++) {
        sqlite3_bind_text(stmt, 5 + i, entry.hashes[i].c_str(), -1, SQLITE_STATIC);
    }
    sqlite3_bind_text(stmt, 10, entry.title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, entry.subtitle.c_str(), -1, SQLITE_STATIC);
    const std::string titles = encode_pairs(entry.titles), subtitles = encode_pairs(entry.subtitles);
    const std::string levels = encode_levels(entry.levels);
    sqlite3_bind_text(stmt, 12, titles.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 13, subtitles.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 14, levels.c_str(), -1, SQLITE_STATIC);
    if (entry.ft_write != 0) {
        sqlite3_bind_int64(stmt, 15, entry.ft_write);
    } else {
        sqlite3_bind_null(stmt, 15);
    }
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

void ScoresManager::set_song_cache_ft_write(const std::vector<std::pair<std::string, int64_t>>& rows) {
    if (rows.empty()) {
        return;
    }
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd, "UPDATE song_cache SET ft_write = ? WHERE path = ?;", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        spdlog::error("set_song_cache_ft_write: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    for (const auto& [path, ft_write] : rows) {
        sqlite3_bind_int64(stmt, 1, ft_write);
        sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void ScoresManager::remove_song_cache(const std::string& path) {
    if (!remove_song_cache_stmt && sqlite3_prepare_v2(db_fsd, "DELETE FROM song_cache WHERE path = ?;", -1,
                                                      &remove_song_cache_stmt, nullptr) != SQLITE_OK) {
        remove_song_cache_stmt = nullptr;
        return;
    }
    sqlite3_stmt* stmt = remove_song_cache_stmt;
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    // and its box record (a file that is gone; rare enough to prepare each time)
    ensure_box_record_table(db_fsd);
    sqlite3_stmt* box;
    if (sqlite3_prepare_v2(db_fsd, "DELETE FROM zdon_song_box_records WHERE path = ?;", -1, &box, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_text(box, 1, path.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(box);
        sqlite3_finalize(box);
    }
}

void ScoresManager::add_song(const std::array<std::string, 5>& hashes, const std::string& title,
                             const std::string& subtitle) {
    // prepared once: the loading screen calls this for every chart in the library
    if (!add_song_stmt) {
        const char* query = "INSERT OR IGNORE INTO songs (title, subtitle, hash_0, hash_1, hash_2, hash_3, hash_4) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_fsd, query, -1, &add_song_stmt, nullptr) != SQLITE_OK) {
            spdlog::error("add_song: failed to prepare statement: {}", sqlite3_errmsg(db_fsd));
            add_song_stmt = nullptr;
            return;
        }
    }
    sqlite3_stmt* stmt = add_song_stmt;
    sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, subtitle.c_str(), -1, SQLITE_STATIC);
    for (int i = 0; i < 5; i++) {
        sqlite3_bind_text(stmt, 3 + i, hashes[i].c_str(), -1, SQLITE_STATIC);
    }
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}

std::pair<std::string, std::string> ScoresManager::get_song_title(const std::string& diff_hash) {
    std::pair<std::string, std::string> out;
    sqlite3_stmt* stmt = nullptr;
    const char* query = "SELECT title, subtitle FROM songs WHERE ?1 IN (hash_0, hash_1, hash_2, hash_3, hash_4) "
                        "LIMIT 1;";
    if (diff_hash.empty() || sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_text(stmt, 1, diff_hash.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* title = sqlite3_column_text(stmt, 0);
        const auto* subtitle = sqlite3_column_text(stmt, 1);
        out.first = title ? reinterpret_cast<const char*>(title) : "";
        out.second = subtitle ? reinterpret_cast<const char*>(subtitle) : "";
    }
    sqlite3_finalize(stmt);
    return out;
}

std::optional<PlayerData> ScoresManager::get_player_data(const std::string& player_id) {
    if (is_guest_id(player_id)) {
        auto [it, fresh] = guest_data.try_emplace(player_id);
        if (fresh) {
            PlayerData& pd = it->second;
            pd.player_id = player_id;
            pd.username = global_data.config->general.language == "ja" ? "ゲスト" : "Guest";
            pd.title = "Donder Debut!";
            if (player_id == guest_id(1)) { // the second seat gets the 2P colours, as a fresh "1" account does
                pd.title_bg = 2;
                std::swap(pd.chara_color_1, pd.chara_color_2);
            }
        }
        return it->second;
    }
    sqlite3_stmt* stmt;
    const char* query = "SELECT player_id, username, title, title_bg, dan, gold, rainbow,"
                        " modifier_auto, modifier_speed, modifier_display, modifier_inverse, modifier_random,"
                        " neiro_index, chara_color_1, chara_color_2, chara_color_3,"
                        " chara_head_index, chara_body_index, chara_cos_index, chara_is_costume,"
                        " chara_paint_index, chara_face_index, chara_acce_index, modifier_skip"
                        " FROM players WHERE player_id = ?;";

    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("get_player: failed to prepare: {}", sqlite3_errmsg(db_fsd));
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);

        // Player not found â€” insert a default row with the requested player_id
        sqlite3_stmt* ins;
        const char* ins_query =
            "INSERT OR IGNORE INTO players (player_id, username, title, title_bg, chara_color_1, chara_color_2) "
            "VALUES (?, ?, 'Donder Debut!', CASE WHEN ?1 = '1' THEN 2 ELSE 0 END, "
            "CASE WHEN ?1 = '1' THEN '#F94728' ELSE '#68BFC0' END, CASE WHEN ?1 = '1' THEN '#68BFC0' ELSE '#F94728' "
            "END);";
        if (sqlite3_prepare_v2(db_fsd, ins_query, -1, &ins, nullptr) == SQLITE_OK) {
            std::string default_name = player_id == "0"   ? "Don-chan"
                                       : player_id == "1" ? "Kat-chan"
                                                          : "Player " + player_id;
            // username is UNIQUE: a migrated account may already own "Don-chan"/"Kat-chan", so suffix on collision
            for (int n = 1; n <= 50; n++) {
                std::string name = n == 1 ? default_name : default_name + " " + std::to_string(n);
                sqlite3_reset(ins);
                sqlite3_bind_text(ins, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(ins, 2, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(ins);
                if (sqlite3_changes(db_fsd) > 0) {
                    break;
                }
            }
            sqlite3_finalize(ins);
        } else {
            spdlog::error("get_player: failed to insert default player {}: {}", player_id, sqlite3_errmsg(db_fsd));
            return std::nullopt;
        }

        // Re-query the newly inserted row
        if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
            spdlog::error("get_player: failed to re-prepare after insert: {}", sqlite3_errmsg(db_fsd));
            return std::nullopt;
        }
        sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return std::nullopt;
        }
    }

    auto col_str = [&](int i) -> std::string {
        auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        return p ? p : "";
    };

    PlayerData p;
    p.player_id = col_str(0);
    p.username = col_str(1);
    p.title = col_str(2);
    p.title_bg = sqlite3_column_int(stmt, 3);
    p.dan = sqlite3_column_int(stmt, 4);
    p.gold = sqlite3_column_int(stmt, 5);
    p.rainbow = sqlite3_column_int(stmt, 6);
    p.modifier_auto = sqlite3_column_int(stmt, 7);
    p.modifier_speed = sqlite3_column_int(stmt, 8);
    p.modifier_display = sqlite3_column_int(stmt, 9);
    p.modifier_inverse = sqlite3_column_int(stmt, 10);
    p.modifier_random = sqlite3_column_int(stmt, 11);
    p.neiro_index = sqlite3_column_int(stmt, 12);
    p.chara_color_1 = parse_hex_color(col_str(13));
    p.chara_color_2 = parse_hex_color(col_str(14));
    p.chara_color_3 = parse_hex_color(col_str(15));
    p.chara_head_index = sqlite3_column_int(stmt, 16);
    p.chara_body_index = sqlite3_column_int(stmt, 17);
    p.chara_cos_index = sqlite3_column_int(stmt, 18);
    p.chara_is_costume = sqlite3_column_int(stmt, 19);
    p.chara_paint_index = sqlite3_column_int(stmt, 20);
    p.chara_face_index = sqlite3_column_int(stmt, 21);
    p.chara_acce_index = sqlite3_column_int(stmt, 22);
    p.modifier_skip = sqlite3_column_int(stmt, 23);

    sqlite3_finalize(stmt);
    p.modifier_equal_speed = get_zdon_option(player_id, "equal_speed") != 0;
    p.modifier_zdon_fade = std::clamp(get_zdon_option(player_id, "fade"), 0, 6);
    p.modifier_ghost = get_zdon_option(player_id, "ghost") != 0;
    p.modifier_sudden_death = std::clamp(get_zdon_option(player_id, "sudden_death"), 0, 2);
    p.modifier_flashlight = get_zdon_option(player_id, "flashlight") != 0;
    return p;
}

std::vector<std::string> ScoresManager::get_player_ids() {
    std::vector<std::string> ids;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_fsd, "SELECT player_id FROM players ORDER BY rowid;", -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("get_player_ids: failed to prepare: {}", sqlite3_errmsg(db_fsd));
        return ids;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ids.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return ids;
}

void ScoresManager::clear_guests() {
    guest_data.clear();
    guest_seen.clear();
}

void ScoresManager::save_player_data(const PlayerData& player) {
    if (is_guest_id(player.player_id)) {
        guest_data[player.player_id] = player;
        return;
    }
    sqlite3_stmt* stmt;
    const char* query =
        "UPDATE players SET"
        " username = ?, title = ?, title_bg = ?, dan = ?, gold = ?, rainbow = ?,"
        " modifier_auto = ?, modifier_speed = ?, modifier_display = ?, modifier_inverse = ?, modifier_random = ?,"
        " neiro_index = ?, chara_color_1 = ?, chara_color_2 = ?, chara_color_3 = ?,"
        " chara_head_index = ?, chara_body_index = ?, chara_cos_index = ?, chara_is_costume = ?,"
        " chara_paint_index = ?, chara_face_index = ?, chara_acce_index = ?, modifier_skip = ?"
        " WHERE player_id = ?;";

    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("save_player: failed to prepare: {}", sqlite3_errmsg(db_fsd));
        return;
    }

    std::string c1 = color_to_hex(player.chara_color_1);
    std::string c2 = color_to_hex(player.chara_color_2);
    std::string c3 = color_to_hex(player.chara_color_3);

    sqlite3_bind_text(stmt, 1, player.username.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, player.title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, player.title_bg);
    sqlite3_bind_int(stmt, 4, player.dan);
    sqlite3_bind_int(stmt, 5, player.gold);
    sqlite3_bind_int(stmt, 6, player.rainbow);
    sqlite3_bind_int(stmt, 7, player.modifier_auto);
    sqlite3_bind_int(stmt, 8, player.modifier_speed);
    sqlite3_bind_int(stmt, 9, player.modifier_display);
    sqlite3_bind_int(stmt, 10, player.modifier_inverse);
    sqlite3_bind_int(stmt, 11, player.modifier_random);
    sqlite3_bind_int(stmt, 12, player.neiro_index);
    sqlite3_bind_text(stmt, 13, c1.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 14, c2.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 15, c3.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 16, player.chara_head_index);
    sqlite3_bind_int(stmt, 17, player.chara_body_index);
    sqlite3_bind_int(stmt, 18, player.chara_cos_index);
    sqlite3_bind_int(stmt, 19, player.chara_is_costume);
    sqlite3_bind_int(stmt, 20, player.chara_paint_index);
    sqlite3_bind_int(stmt, 21, player.chara_face_index);
    sqlite3_bind_int(stmt, 22, player.chara_acce_index);
    sqlite3_bind_int(stmt, 23, player.modifier_skip);
    sqlite3_bind_text(stmt, 24, player.player_id.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("save_player: failed to update player {}: {}", player.player_id, sqlite3_errmsg(db_fsd));
    }

    sqlite3_finalize(stmt);
    set_zdon_option(player.player_id, "equal_speed", player.modifier_equal_speed ? 1 : 0);
    set_zdon_option(player.player_id, "fade", player.modifier_zdon_fade);
    set_zdon_option(player.player_id, "ghost", player.modifier_ghost ? 1 : 0);
    set_zdon_option(player.player_id, "sudden_death", player.modifier_sudden_death);
    set_zdon_option(player.player_id, "flashlight", player.modifier_flashlight ? 1 : 0);
}

static void ensure_dan_table(sqlite3* db) {
    sqlite3_exec(db,
                 "CREATE TABLE IF NOT EXISTS dan_scores"
                 "(dan_score_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "player_id TEXT NOT NULL,"
                 "course_id TEXT NOT NULL,"
                 "title TEXT NOT NULL,"
                 "dan_index INTEGER NOT NULL DEFAULT -1,"
                 "odai_result INTEGER NOT NULL,"
                 "score INTEGER NOT NULL,"
                 "max_combo INTEGER NOT NULL DEFAULT 0,"
                 "gauge_length REAL NOT NULL DEFAULT 0,"
                 "arrival INTEGER NOT NULL DEFAULT 0,"
                 "played_at INTEGER NOT NULL DEFAULT 0);"
                 "CREATE INDEX IF NOT EXISTS idx_dan_scores_course ON dan_scores(player_id, course_id);"
                 "CREATE TABLE IF NOT EXISTS dan_score_songs"
                 "(dan_score_id INTEGER NOT NULL REFERENCES dan_scores(dan_score_id) ON DELETE CASCADE,"
                 "song_index INTEGER NOT NULL,"
                 "score_id INTEGER REFERENCES scores(score_id),"
                 "PRIMARY KEY(dan_score_id, song_index));",
                 nullptr, nullptr, nullptr);
    // Z-DON: the exam bests as of each attempt (fails harmlessly once the column exists)
    sqlite3_exec(db, "ALTER TABLE dan_scores ADD COLUMN exam_bests TEXT NOT NULL DEFAULT '';", nullptr, nullptr,
                 nullptr);
}

// exam_bests column: one "type:value,s1,s2,s3" per exam, joined by ';' (-1 = none)
static std::string encode_exam_bests(const std::vector<DanExamBest>& bests) {
    std::string out;
    for (size_t i = 0; i < bests.size(); i++) {
        const DanExamBest& b = bests[i];
        if (i) {
            out += ';';
        }
        out += b.type + ':' + std::to_string(b.value);
        for (int v : b.song) {
            out += ',' + std::to_string(v);
        }
    }
    return out;
}

static std::vector<DanExamBest> decode_exam_bests(const std::string& s) {
    std::vector<DanExamBest> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(';', start);
        if (end == std::string::npos) {
            end = s.size();
        }
        const std::string item = s.substr(start, end - start);
        start = end + 1;
        DanExamBest b;
        size_t colon = item.find(':');
        if (colon == std::string::npos) {
            out.push_back(b);
            continue;
        }
        b.type = item.substr(0, colon);
        int vals[4] = {-1, -1, -1, -1};
        size_t pos = colon + 1;
        for (int k = 0; k < 4 && pos <= item.size(); k++) {
            size_t comma = item.find(',', pos);
            if (comma == std::string::npos) {
                comma = item.size();
            }
            try {
                vals[k] = std::stoi(item.substr(pos, comma - pos));
            } catch (...) {
                vals[k] = -1;
            }
            pos = comma + 1;
        }
        b.value = vals[0];
        for (int k = 0; k < 3; k++) {
            b.song[k] = vals[k + 1];
        }
        out.push_back(b);
    }
    return out;
}

std::string dan_song_hash(const DanSongEntry& song) {
    const int diff = std::clamp(song.difficulty, 0, 4);
    std::string h = scores_manager.get_hashes(song.song_path)[diff];
    if (h.empty()) {
        try {
            h = SongParser(song.song_path).get_diff_hash(diff);
        } catch (...) {
            spdlog::warn("dan_song_hash: could not read {}", song.song_path.string());
        }
    }
    return h;
}

std::string dan_course_id(const std::vector<DanSongEntry>& songs, const std::vector<Exam>& exams) {
    std::string key;
    for (const auto& s : songs) {
        key += dan_song_hash(s) + "|" + std::to_string(s.difficulty) + ";";
    }
    for (const auto& e : exams) {
        key += "E" + e.type + "," + std::to_string(e.red) + "," + std::to_string(e.gold) + "," + e.range + "," +
               std::to_string(e.gothrough);
        for (int v : e.song_red) {
            key += ",r" + std::to_string(v);
        }
        for (int v : e.song_gold) {
            key += ",g" + std::to_string(v);
        }
        key += ';';
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint8_t b : crypto::sha256(key)) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

std::optional<DanRecord> ScoresManager::get_dan_record(const std::string& player_id, const std::string& course_id) {
    sqlite3_stmt* stmt;
    // rank: 1 = played/failed, 2..7 = passes (odai_result + 1)
    const char* query = "SELECT MAX(dan_index), MAX(odai_result) + 1, MAX(score), MAX(arrival)"
                        " FROM dan_scores WHERE player_id = ? AND course_id = ?;";
    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("get_dan_record: failed to prepare: {}", sqlite3_errmsg(db_fsd));
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, course_id.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<DanRecord> out;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
        DanRecord r;
        r.dan_index = sqlite3_column_int(stmt, 0);
        r.rank = sqlite3_column_int(stmt, 1);
        r.score = sqlite3_column_int(stmt, 2);
        r.arrival = sqlite3_column_int(stmt, 3);
        out = r;
    }
    sqlite3_finalize(stmt);
    if (out && sqlite3_prepare_v2(db_fsd,
                                  "SELECT exam_bests FROM dan_scores WHERE player_id = ? AND course_id = ?"
                                  " ORDER BY dan_score_id DESC LIMIT 1;",
                                  -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, course_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (const unsigned char* eb = sqlite3_column_text(stmt, 0)) {
                out->exam_bests = decode_exam_bests(reinterpret_cast<const char*>(eb));
            }
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

void ScoresManager::save_ai_section_wins(const std::string& hash, int difficulty, const std::string& player_id, int won,
                                         int total) {
    if (is_guest_id(player_id)) {
        return;
    }
    sqlite3_exec(db_fsd,
                 "CREATE TABLE IF NOT EXISTS ai_section_wins"
                 "(hash TEXT NOT NULL, difficulty INTEGER NOT NULL, player_id TEXT NOT NULL,"
                 "sections_won INTEGER NOT NULL, sections_total INTEGER NOT NULL,"
                 "PRIMARY KEY(hash, difficulty, player_id));",
                 nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt;
    const char* query = "INSERT INTO ai_section_wins (hash, difficulty, player_id, sections_won, sections_total)"
                        " VALUES (?, ?, ?, ?, ?) ON CONFLICT DO UPDATE SET"
                        " sections_won = MAX(sections_won, excluded.sections_won),"
                        " sections_total = excluded.sections_total;";
    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("save_ai_section_wins: failed to prepare: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, difficulty);
    sqlite3_bind_text(stmt, 3, player_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, won);
    sqlite3_bind_int(stmt, 5, total);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        spdlog::error("save_ai_section_wins: failed for {}: {}", hash, sqlite3_errmsg(db_fsd));
    }
    sqlite3_finalize(stmt);
}

void ScoresManager::save_dan_score(const std::string& player_id, const std::string& course_id, const DanResultData& rd,
                                   int arrival, const std::vector<DanExamBest>& exam_bests) {
    if (is_guest_id(player_id)) {
        return;
    }
    sqlite3_stmt* stmt;
    const char* query = "INSERT INTO dan_scores (player_id, course_id, title, dan_index, odai_result, score, "
                        "max_combo, gauge_length, arrival, played_at, exam_bests)"
                        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_fsd, query, -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("save_dan_score: failed to prepare: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, course_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, rd.dan_title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, rd.course.index);
    sqlite3_bind_int(stmt, 5, rd.odai_result);
    sqlite3_bind_int(stmt, 6, rd.score);
    sqlite3_bind_int(stmt, 7, rd.max_combo);
    sqlite3_bind_double(stmt, 8, rd.gauge_length);
    sqlite3_bind_int(stmt, 9, arrival);
    sqlite3_bind_int64(stmt, 10, unix_now());
    const std::string bests = encode_exam_bests(exam_bests);
    sqlite3_bind_text(stmt, 11, bests.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) {
        spdlog::error("save_dan_score: failed for {}: {}", rd.dan_title, sqlite3_errmsg(db_fsd));
    }
    sqlite3_finalize(stmt);
    if (!ok) {
        return;
    }

    const sqlite3_int64 id = sqlite3_last_insert_rowid(db_fsd);
    if (sqlite3_prepare_v2(db_fsd, "INSERT INTO dan_score_songs (dan_score_id, song_index, score_id) VALUES (?, ?, ?);",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        spdlog::error("save_dan_score: songs prepare failed: {}", sqlite3_errmsg(db_fsd));
        return;
    }
    for (int i = 0; i < std::ssize(rd.songs); i++) {
        const auto& s = rd.songs[i];
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_int(stmt, 2, i);
        if (s.score_id >= 0) {
            sqlite3_bind_int64(stmt, 3, s.score_id);
        } else {
            sqlite3_bind_null(stmt, 3);
        }
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

bool ScoresManager::first_visit(const std::string& player_id, const char* seen_column) {
    if (is_guest_id(player_id)) {
        return guest_seen.insert(player_id + seen_column).second;
    }
    sqlite3_stmt* stmt;
    const std::string query = std::format("UPDATE players SET {0} = 1 WHERE player_id = ? AND {0} = 0;", seen_column);
    if (sqlite3_prepare_v2(db_fsd, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, player_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool first = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_fsd) == 1;
    sqlite3_finalize(stmt);
    return first;
}

void ScoresManager::migrate_player_id(const std::string& old_id, const std::string& new_id) {
    if (old_id == new_id) {
        return;
    }

    spdlog::info("Migrating player ID from {} to {}", old_id, new_id);

    // Check if new_id already exists
    sqlite3_stmt* stmt;
    const char* check_query = "SELECT 1 FROM players WHERE player_id = ?;";
    if (sqlite3_prepare_v2(db_fsd, check_query, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, new_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            spdlog::warn("Player ID {} already exists, skipping migration", new_id);
            sqlite3_finalize(stmt);
            return;
        }
        sqlite3_finalize(stmt);
    }

    for (const char* sql : {"UPDATE players SET player_id = ? WHERE player_id = ?;",
                            "UPDATE scores SET player_id = ? WHERE player_id = ?;",
                            "UPDATE dan_scores SET player_id = ? WHERE player_id = ?;"}) {
        if (sqlite3_prepare_v2(db_fsd, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, new_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, old_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

bool ScoresManager::begin_transaction() {
    if (sqlite3_exec(db_fsd, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        spdlog::error("begin_transaction failed: {}", sqlite3_errmsg(db_fsd));
        return false;
    }
    return true;
}

bool ScoresManager::commit() {
    if (sqlite3_exec(db_fsd, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        spdlog::error("commit failed: {}", sqlite3_errmsg(db_fsd));
        return false;
    }
    return true;
}

ScoresManager* _scores_manager_ptr = nullptr;

void init_scores_manager(bool gen3) {
    delete _scores_manager_ptr; // calling this more than once would otherwise leak the previous manager
    _scores_manager_ptr = new ScoresManager(gen3 ? "scores_gen3.db" : "scores.db");
}
