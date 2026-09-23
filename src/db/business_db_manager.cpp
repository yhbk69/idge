/* 蔡超添加
数据库相关管理。 
*/
#include "business_db_manager.h"
#include <iostream>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <utility>

BusinessDBManager::BusinessDBManager() : db_(nullptr), initialized_(false) {}

BusinessDBManager::~BusinessDBManager() {
    close();
}

bool BusinessDBManager::open(const std::string& db_path) {
    int ret = sqlite3_open(db_path.c_str(), &db_);
    if (ret != SQLITE_OK) {
        std::cerr << "Failed to open database: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    if (!createTables()) {
        close();
        return false;
    }
    
    initialized_ = true;
    return true;
}

void BusinessDBManager::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    initialized_ = false;
}

bool BusinessDBManager::createTables() {
    const char* create_tasks_table = R"(
        CREATE TABLE IF NOT EXISTS tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            type TEXT NOT NULL,
            create_time TEXT NOT NULL,
            folder_path TEXT,
            registered_count INTEGER DEFAULT 0,
            cancelled_count INTEGER DEFAULT 0,
            is_cancelled INTEGER DEFAULT 0,
            parent_task_id INTEGER DEFAULT 0
        );
    )";
    
    const char* create_face_records_table = R"(
        CREATE TABLE IF NOT EXISTS face_records (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            task_id INTEGER NOT NULL,
            feature_blob BLOB,
            original_photo_path TEXT,
            processed_photo_path TEXT,
            face_photo_path TEXT,
            face_index INTEGER DEFAULT 0,
            is_duplicate INTEGER DEFAULT 0,
            similar_to_id INTEGER DEFAULT 0,
            FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE
        );
    )";

    const char* create_cancellation_photos_table = R"(
        CREATE TABLE IF NOT EXISTS cancellation_photos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            task_id INTEGER NOT NULL,
            original_photo_path TEXT NOT NULL DEFAULT '',
            processed_photo_path TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE
        );
    )";

    const char* create_cancellation_matches_table = R"(
        CREATE TABLE IF NOT EXISTS cancellation_matches (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            task_id INTEGER NOT NULL,
            registration_record_id INTEGER DEFAULT 0,
            registration_image TEXT NOT NULL DEFAULT '',
            cancellation_image TEXT NOT NULL DEFAULT '',
            similarity REAL DEFAULT 0,
            status INTEGER DEFAULT 0,
            FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE
        );
    )";

    const char* create_equipment_photos_table = R"(
        CREATE TABLE IF NOT EXISTS equipment_photos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            task_id INTEGER NOT NULL,
            phase INTEGER NOT NULL DEFAULT 0,
            original_photo_path TEXT NOT NULL DEFAULT '',
            processed_photo_path TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE
        );
    )";

    const char* create_equipment_detections_table = R"(
        CREATE TABLE IF NOT EXISTS equipment_detections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            photo_id INTEGER NOT NULL,
            class_index INTEGER NOT NULL DEFAULT 0,
            label TEXT NOT NULL DEFAULT '',
            confidence REAL NOT NULL DEFAULT 0,
            x1 INTEGER NOT NULL DEFAULT 0,
            y1 INTEGER NOT NULL DEFAULT 0,
            x2 INTEGER NOT NULL DEFAULT 0,
            y2 INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (photo_id) REFERENCES equipment_photos(id) ON DELETE CASCADE
        );
    )";
    
    char* err_msg = nullptr;
    
    if (sqlite3_exec(db_, create_tasks_table, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create tasks table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    if (sqlite3_exec(db_, create_face_records_table, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create face_records table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(db_, create_cancellation_photos_table, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create cancellation_photos table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(db_, create_cancellation_matches_table, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create cancellation_matches table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(db_, create_equipment_photos_table, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create equipment_photos table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    if (sqlite3_exec(db_, create_equipment_detections_table, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create equipment_detections table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
}

int BusinessDBManager::createTask(const std::string& name, const std::string& type, const std::string& folder_path) {
    if (!initialized_) return -1;
    
    // 获取当前时间
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    std::string create_time = oss.str();
    
    const char* sql = "INSERT INTO tasks (name, type, create_time, folder_path) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, create_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, folder_path.c_str(), -1, SQLITE_TRANSIENT);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Failed to insert task: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_finalize(stmt);
        return -1;
    }
    
    int task_id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    
    return task_id;
}

bool BusinessDBManager::updateTask(int task_id, int registered_count, int cancelled_count, int is_cancelled) {
    if (!initialized_) return false;
    
    const char* sql = "UPDATE tasks SET registered_count = ?, cancelled_count = ?, is_cancelled = ? WHERE id = ?;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, registered_count);
    sqlite3_bind_int(stmt, 2, cancelled_count);
    sqlite3_bind_int(stmt, 3, is_cancelled);
    sqlite3_bind_int(stmt, 4, task_id);
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    
    return success;
}

bool BusinessDBManager::deleteTask(int task_id) {
    if (!initialized_) return false;

    deleteCancellationDataByTask(task_id);
    deleteEquipmentDataByTask(task_id);
    
    // 先删除关联的人脸记录
    deleteFaceRecordsByTask(task_id);
    
    const char* sql = "DELETE FROM tasks WHERE id = ?;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, task_id);
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    
    return success;
}

Task BusinessDBManager::getTask(int task_id) {
    Task task = {0};
    if (!initialized_) return task;
    
    const char* sql = "SELECT * FROM tasks WHERE id = ?;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return task;
    }
    
    sqlite3_bind_int(stmt, 1, task_id);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        task.id = sqlite3_column_int(stmt, 0);
        task.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        task.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        task.create_time = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        task.folder_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        task.registered_count = sqlite3_column_int(stmt, 5);
        task.cancelled_count = sqlite3_column_int(stmt, 6);
        task.is_cancelled = sqlite3_column_int(stmt, 7);
        task.parent_task_id = sqlite3_column_int(stmt, 8);
    }
    
    sqlite3_finalize(stmt);
    return task;
}

std::vector<Task> BusinessDBManager::getAllTasks() {
    std::vector<Task> tasks;
    if (!initialized_) return tasks;
    
    const char* sql = "SELECT * FROM tasks ORDER BY create_time DESC;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return tasks;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Task task;
        task.id = sqlite3_column_int(stmt, 0);
        task.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        task.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        task.create_time = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        task.folder_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        task.registered_count = sqlite3_column_int(stmt, 5);
        task.cancelled_count = sqlite3_column_int(stmt, 6);
        task.is_cancelled = sqlite3_column_int(stmt, 7);
        task.parent_task_id = sqlite3_column_int(stmt, 8);
        tasks.push_back(task);
    }
    
    sqlite3_finalize(stmt);
    return tasks;
}

std::vector<Task> BusinessDBManager::getRegistrationTasks() {
    std::vector<Task> tasks;
    if (!initialized_) return tasks;
    
    const char* sql = "SELECT * FROM tasks WHERE type = 'registration' ORDER BY create_time DESC;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return tasks;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Task task;
        task.id = sqlite3_column_int(stmt, 0);
        task.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        task.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        task.create_time = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        task.folder_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        task.registered_count = sqlite3_column_int(stmt, 5);
        task.cancelled_count = sqlite3_column_int(stmt, 6);
        task.is_cancelled = sqlite3_column_int(stmt, 7);
        task.parent_task_id = sqlite3_column_int(stmt, 8);
        tasks.push_back(task);
    }
    
    sqlite3_finalize(stmt);
    return tasks;
}

int BusinessDBManager::insertFaceRecord(const FaceRecord& record) {
    if (!initialized_) return -1;
    
    const char* sql = R"(
        INSERT INTO face_records 
        (task_id, feature_blob, original_photo_path, processed_photo_path, 
         face_photo_path, face_index, is_duplicate, similar_to_id) 
        VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )";
    
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }
    
    std::vector<uint8_t> blob = featureToBlob(record.feature);
    
    sqlite3_bind_int(stmt, 1, record.task_id);
    sqlite3_bind_blob(stmt, 2, blob.data(), blob.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.original_photo_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.processed_photo_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, record.face_photo_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, record.face_index);
    sqlite3_bind_int(stmt, 7, record.is_duplicate);
    sqlite3_bind_int(stmt, 8, record.similar_to_id);
    
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    
    int record_id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(stmt);
    
    return record_id;
}

bool BusinessDBManager::deleteFaceRecordsByTask(int task_id) {
    if (!initialized_) return false;
    
    const char* sql = "DELETE FROM face_records WHERE task_id = ?;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, task_id);
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    
    return success;
}

std::vector<FaceRecord> BusinessDBManager::getFaceRecordsByTask(int task_id) {
    std::vector<FaceRecord> records;
    if (!initialized_) return records;
    
    const char* sql = "SELECT * FROM face_records WHERE task_id = ?;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return records;
    }
    
    sqlite3_bind_int(stmt, 1, task_id);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FaceRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.task_id = sqlite3_column_int(stmt, 1);
        
        const void* blob = sqlite3_column_blob(stmt, 2);
        int blob_size = sqlite3_column_bytes(stmt, 2);
        record.feature = blobToFeature(blob, blob_size);
        
        record.original_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.processed_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        record.face_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        record.face_index = sqlite3_column_int(stmt, 6);
        record.is_duplicate = sqlite3_column_int(stmt, 7);
        record.similar_to_id = sqlite3_column_int(stmt, 8);
        
        records.push_back(record);
    }
    
    sqlite3_finalize(stmt);
    return records;
}

std::vector<FaceRecord> BusinessDBManager::getUniqueFacesByTask(int task_id) {
    std::vector<FaceRecord> records;
    if (!initialized_) return records;
    
    const char* sql = "SELECT * FROM face_records WHERE task_id = ? AND is_duplicate = 0;";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return records;
    }
    
    sqlite3_bind_int(stmt, 1, task_id);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FaceRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.task_id = sqlite3_column_int(stmt, 1);
        
        const void* blob = sqlite3_column_blob(stmt, 2);
        int blob_size = sqlite3_column_bytes(stmt, 2);
        record.feature = blobToFeature(blob, blob_size);
        
        record.original_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.processed_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        record.face_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        record.face_index = sqlite3_column_int(stmt, 6);
        record.is_duplicate = sqlite3_column_int(stmt, 7);
        record.similar_to_id = sqlite3_column_int(stmt, 8);
        
        records.push_back(record);
    }
    
    sqlite3_finalize(stmt);
    return records;
}

std::vector<float> BusinessDBManager::blobToFeature(const void* blob, int size) {
    std::vector<float> feature;
    if (!blob || size == 0) return feature;
    
    int num_floats = size / sizeof(float);
    const float* data = static_cast<const float*>(blob);
    feature.assign(data, data + num_floats);
    
    return feature;
}

std::vector<uint8_t> BusinessDBManager::featureToBlob(const std::vector<float>& feature) {
    std::vector<uint8_t> blob;
    if (feature.empty()) return blob;
    
    const uint8_t* data = reinterpret_cast<const uint8_t*>(feature.data());
    size_t size = feature.size() * sizeof(float);
    blob.assign(data, data + size);
    
    return blob;
}

bool BusinessDBManager::updateFaceRecord(int record_id, const FaceRecord& record) {
    if (!initialized_) return false;
    
    const char* sql = R"(
        UPDATE face_records 
        SET original_photo_path = ?, processed_photo_path = ?, 
            face_photo_path = ?, is_duplicate = ?, similar_to_id = ?
        WHERE id = ?;
    )";
    
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, record.original_photo_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.processed_photo_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.face_photo_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, record.is_duplicate);
    sqlite3_bind_int(stmt, 5, record.similar_to_id);
    sqlite3_bind_int(stmt, 6, record_id);
    
    bool success = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    
    return success;
}

bool BusinessDBManager::replaceCancellationData(
    int task_id,
    const std::vector<CancellationPhotoRecord>& photos,
    const std::vector<CancellationMatchRecord>& matches) {
    if (!initialized_) return false;

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;

    auto rollback = [this]() {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    if (!deleteCancellationDataByTask(task_id)) {
        rollback();
        return false;
    }

    const char* photo_sql = R"(
        INSERT INTO cancellation_photos
        (task_id, original_photo_path, processed_photo_path)
        VALUES (?, ?, ?);
    )";
    sqlite3_stmt* photo_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, photo_sql, -1, &photo_stmt, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    for (const auto& photo : photos) {
        sqlite3_bind_int(photo_stmt, 1, task_id);
        sqlite3_bind_text(photo_stmt, 2, photo.original_photo_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(photo_stmt, 3, photo.processed_photo_path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(photo_stmt) != SQLITE_DONE) {
            sqlite3_finalize(photo_stmt);
            rollback();
            return false;
        }
        sqlite3_reset(photo_stmt);
        sqlite3_clear_bindings(photo_stmt);
    }
    sqlite3_finalize(photo_stmt);

    const char* match_sql = R"(
        INSERT INTO cancellation_matches
        (task_id, registration_record_id, registration_image,
         cancellation_image, similarity, status)
        VALUES (?, ?, ?, ?, ?, ?);
    )";
    sqlite3_stmt* match_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, match_sql, -1, &match_stmt, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    for (const auto& match : matches) {
        sqlite3_bind_int(match_stmt, 1, task_id);
        sqlite3_bind_int(match_stmt, 2, match.registration_record_id);
        sqlite3_bind_text(match_stmt, 3, match.registration_image.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(match_stmt, 4, match.cancellation_image.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(match_stmt, 5, match.similarity);
        sqlite3_bind_int(match_stmt, 6, match.status);
        if (sqlite3_step(match_stmt) != SQLITE_DONE) {
            sqlite3_finalize(match_stmt);
            rollback();
            return false;
        }
        sqlite3_reset(match_stmt);
        sqlite3_clear_bindings(match_stmt);
    }
    sqlite3_finalize(match_stmt);

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    return true;
}

bool BusinessDBManager::deleteCancellationDataByTask(int task_id) {
    if (!initialized_) return false;

    auto delete_from = [this, task_id](const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_int(stmt, 1, task_id);
        const bool success = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return success;
    };

    const bool matches_deleted = delete_from(
        "DELETE FROM cancellation_matches WHERE task_id = ?;");
    const bool photos_deleted = delete_from(
        "DELETE FROM cancellation_photos WHERE task_id = ?;");
    return matches_deleted && photos_deleted;
}

std::vector<CancellationPhotoRecord>
BusinessDBManager::getCancellationPhotosByTask(int task_id) {
    std::vector<CancellationPhotoRecord> photos;
    if (!initialized_) return photos;

    const char* sql = R"(
        SELECT id, task_id, original_photo_path, processed_photo_path
        FROM cancellation_photos WHERE task_id = ? ORDER BY id;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return photos;
    sqlite3_bind_int(stmt, 1, task_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CancellationPhotoRecord photo;
        photo.id = sqlite3_column_int(stmt, 0);
        photo.task_id = sqlite3_column_int(stmt, 1);
        photo.original_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        photo.processed_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        photos.push_back(std::move(photo));
    }
    sqlite3_finalize(stmt);
    return photos;
}

std::vector<CancellationMatchRecord>
BusinessDBManager::getCancellationMatchesByTask(int task_id) {
    std::vector<CancellationMatchRecord> matches;
    if (!initialized_) return matches;

    const char* sql = R"(
        SELECT id, task_id, registration_record_id, registration_image,
               cancellation_image, similarity, status
        FROM cancellation_matches WHERE task_id = ? ORDER BY id;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return matches;
    sqlite3_bind_int(stmt, 1, task_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CancellationMatchRecord match;
        match.id = sqlite3_column_int(stmt, 0);
        match.task_id = sqlite3_column_int(stmt, 1);
        match.registration_record_id = sqlite3_column_int(stmt, 2);
        match.registration_image = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        match.cancellation_image = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        match.similarity = static_cast<float>(sqlite3_column_double(stmt, 5));
        match.status = sqlite3_column_int(stmt, 6);
        matches.push_back(std::move(match));
    }
    sqlite3_finalize(stmt);
    return matches;
}

std::vector<Task> BusinessDBManager::getTasksByType(const std::string& type) {
    std::vector<Task> tasks;
    if (!initialized_) return tasks;

    const char* sql = "SELECT * FROM tasks WHERE type = ? ORDER BY create_time DESC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return tasks;
    sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Task task;
        task.id = sqlite3_column_int(stmt, 0);
        task.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        task.type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        task.create_time = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        task.folder_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        task.registered_count = sqlite3_column_int(stmt, 5);
        task.cancelled_count = sqlite3_column_int(stmt, 6);
        task.is_cancelled = sqlite3_column_int(stmt, 7);
        task.parent_task_id = sqlite3_column_int(stmt, 8);
        tasks.push_back(std::move(task));
    }
    sqlite3_finalize(stmt);
    return tasks;
}

bool BusinessDBManager::replaceEquipmentData(
    int task_id, int phase,
    const std::vector<EquipmentPhotoRecord>& photos,
    const std::vector<EquipmentDetectionRecord>& detections) {
    if (!initialized_) return false;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;

    auto rollback = [this]() { sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

    const char* delete_detections =
        "DELETE FROM equipment_detections WHERE photo_id IN "
        "(SELECT id FROM equipment_photos WHERE task_id = ? AND phase = ?);";
    sqlite3_stmt* delete_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, delete_detections, -1, &delete_stmt, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    sqlite3_bind_int(delete_stmt, 1, task_id);
    sqlite3_bind_int(delete_stmt, 2, phase);
    if (sqlite3_step(delete_stmt) != SQLITE_DONE) {
        sqlite3_finalize(delete_stmt);
        rollback();
        return false;
    }
    sqlite3_finalize(delete_stmt);

    const char* delete_photos = "DELETE FROM equipment_photos WHERE task_id = ? AND phase = ?;";
    if (sqlite3_prepare_v2(db_, delete_photos, -1, &delete_stmt, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    sqlite3_bind_int(delete_stmt, 1, task_id);
    sqlite3_bind_int(delete_stmt, 2, phase);
    if (sqlite3_step(delete_stmt) != SQLITE_DONE) {
        sqlite3_finalize(delete_stmt);
        rollback();
        return false;
    }
    sqlite3_finalize(delete_stmt);

    const char* photo_sql =
        "INSERT INTO equipment_photos "
        "(task_id, phase, original_photo_path, processed_photo_path) VALUES (?, ?, ?, ?);";
    const char* detection_sql =
        "INSERT INTO equipment_detections "
        "(photo_id, class_index, label, confidence, x1, y1, x2, y2) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* photo_stmt = nullptr;
    sqlite3_stmt* detection_stmt = nullptr;
    if (sqlite3_prepare_v2(db_, photo_sql, -1, &photo_stmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db_, detection_sql, -1, &detection_stmt, nullptr) != SQLITE_OK) {
        if (photo_stmt) sqlite3_finalize(photo_stmt);
        if (detection_stmt) sqlite3_finalize(detection_stmt);
        rollback();
        return false;
    }

    std::vector<int> photo_ids;
    photo_ids.reserve(photos.size());
    for (const auto& photo : photos) {
        sqlite3_bind_int(photo_stmt, 1, task_id);
        sqlite3_bind_int(photo_stmt, 2, phase);
        sqlite3_bind_text(photo_stmt, 3, photo.original_photo_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(photo_stmt, 4, photo.processed_photo_path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(photo_stmt) != SQLITE_DONE) {
            sqlite3_finalize(photo_stmt);
            sqlite3_finalize(detection_stmt);
            rollback();
            return false;
        }
        photo_ids.push_back(static_cast<int>(sqlite3_last_insert_rowid(db_)));
        sqlite3_reset(photo_stmt);
        sqlite3_clear_bindings(photo_stmt);
    }
    sqlite3_finalize(photo_stmt);

    for (const auto& detection : detections) {
        if (detection.photo_id < 0 ||
            detection.photo_id >= static_cast<int>(photo_ids.size())) {
            sqlite3_finalize(detection_stmt);
            rollback();
            return false;
        }
        sqlite3_bind_int(detection_stmt, 1, photo_ids[detection.photo_id]);
        sqlite3_bind_int(detection_stmt, 2, detection.class_index);
        sqlite3_bind_text(detection_stmt, 3, detection.label.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(detection_stmt, 4, detection.confidence);
        sqlite3_bind_int(detection_stmt, 5, detection.x1);
        sqlite3_bind_int(detection_stmt, 6, detection.y1);
        sqlite3_bind_int(detection_stmt, 7, detection.x2);
        sqlite3_bind_int(detection_stmt, 8, detection.y2);
        if (sqlite3_step(detection_stmt) != SQLITE_DONE) {
            sqlite3_finalize(detection_stmt);
            rollback();
            return false;
        }
        sqlite3_reset(detection_stmt);
        sqlite3_clear_bindings(detection_stmt);
    }
    sqlite3_finalize(detection_stmt);

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    return true;
}

std::vector<EquipmentPhotoRecord>
BusinessDBManager::getEquipmentPhotosByTask(int task_id, int phase) {
    std::vector<EquipmentPhotoRecord> photos;
    if (!initialized_) return photos;
    const char* sql =
        "SELECT id, task_id, phase, original_photo_path, processed_photo_path "
        "FROM equipment_photos WHERE task_id = ? AND phase = ? ORDER BY id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return photos;
    sqlite3_bind_int(stmt, 1, task_id);
    sqlite3_bind_int(stmt, 2, phase);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EquipmentPhotoRecord photo;
        photo.id = sqlite3_column_int(stmt, 0);
        photo.task_id = sqlite3_column_int(stmt, 1);
        photo.phase = sqlite3_column_int(stmt, 2);
        photo.original_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        photo.processed_photo_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        photos.push_back(std::move(photo));
    }
    sqlite3_finalize(stmt);
    return photos;
}

std::vector<EquipmentDetectionRecord>
BusinessDBManager::getEquipmentDetectionsByPhoto(int photo_id) {
    std::vector<EquipmentDetectionRecord> detections;
    if (!initialized_) return detections;
    const char* sql =
        "SELECT id, photo_id, class_index, label, confidence, x1, y1, x2, y2 "
        "FROM equipment_detections WHERE photo_id = ? ORDER BY id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return detections;
    sqlite3_bind_int(stmt, 1, photo_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EquipmentDetectionRecord detection;
        detection.id = sqlite3_column_int(stmt, 0);
        detection.photo_id = sqlite3_column_int(stmt, 1);
        detection.class_index = sqlite3_column_int(stmt, 2);
        detection.label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        detection.confidence = static_cast<float>(sqlite3_column_double(stmt, 4));
        detection.x1 = sqlite3_column_int(stmt, 5);
        detection.y1 = sqlite3_column_int(stmt, 6);
        detection.x2 = sqlite3_column_int(stmt, 7);
        detection.y2 = sqlite3_column_int(stmt, 8);
        detections.push_back(std::move(detection));
    }
    sqlite3_finalize(stmt);
    return detections;
}

bool BusinessDBManager::deleteEquipmentDataByTask(int task_id) {
    if (!initialized_) return false;
    const char* detection_sql =
        "DELETE FROM equipment_detections WHERE photo_id IN "
        "(SELECT id FROM equipment_photos WHERE task_id = ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, detection_sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int(stmt, 1, task_id);
    bool success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!success) return false;

    const char* photo_sql = "DELETE FROM equipment_photos WHERE task_id = ?;";
    if (sqlite3_prepare_v2(db_, photo_sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int(stmt, 1, task_id);
    success = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return success;
}
