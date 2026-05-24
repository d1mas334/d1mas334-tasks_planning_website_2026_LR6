#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <userver/components/component_base.hpp>
#include <userver/components/component_context.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/clients/dns/component.hpp>
#include <userver/formats/bson/document.hpp>
#include <userver/formats/bson/types.hpp>
#include <userver/formats/bson/value.hpp>
#include <userver/formats/bson/value_builder.hpp>
#include <userver/formats/common/type.hpp>
#include <userver/formats/json.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/server/http/http_request.hpp>
#include <userver/server/http/http_status.hpp>
#include <userver/storages/mongo/collection.hpp>
#include <userver/storages/mongo/component.hpp>
#include <userver/storages/mongo/options.hpp>
#include <userver/storages/mongo/pool.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/postgres/result_set.hpp>
#include <userver/storages/redis/client.hpp>
#include <userver/storages/redis/component.hpp>
#include <userver/storages/redis/command_control.hpp>
#include <userver/storages/redis/exception.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/provider_component.hpp>
#include <userver/testsuite/testsuite_support.hpp>
#include <userver/utils/datetime.hpp>
#include <userver/utils/daemon_run.hpp>

namespace task_planning {

namespace bson = userver::formats::bson;
namespace components = userver::components;
namespace formats = userver::formats;
namespace handlers = userver::server::handlers;
namespace http = userver::server::http;
namespace json = userver::formats::json;
namespace mongo = userver::storages::mongo;
namespace mongo_options = userver::storages::mongo::options;
namespace postgres = userver::storages::postgres;
namespace redis = userver::storages::redis;
namespace server = userver::server;
namespace datetime = userver::utils::datetime;

struct User {
  std::int64_t id{};
  std::string login;
  std::string password_hash;
  std::string first_name;
  std::string last_name;
  std::string email;
  std::string phone;
  std::string role;
};

struct Goal {
  std::int64_t id{};
  std::string title;
  std::string description;
  std::int64_t author_id{};
  std::string status;
};

struct Task {
  std::int64_t id{};
  std::int64_t goal_id{};
  std::string title;
  std::string description;
  std::int64_t assignee_id{};
  std::int64_t author_id{};
  std::string status;
  std::string due_date;
};

class PostgresStorage final : public components::ComponentBase {
 public:
  static constexpr std::string_view kName = "postgres-storage";

  PostgresStorage(const components::ComponentConfig& config,
                  const components::ComponentContext& context)
      : ComponentBase(config, context),
        pg_cluster_(
            context.FindComponent<components::Postgres>("task-planning-db")
                .GetCluster()) {}

  std::optional<User> CreateUser(const User& user) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          INSERT INTO users (
              login, password_hash, first_name, last_name, email, phone, role
          )
          VALUES ($1, $2, $3, $4, $5, NULLIF($6, ''), $7)
          ON CONFLICT DO NOTHING
          RETURNING id, login, password_hash, first_name, last_name, email,
                    COALESCE(phone, '') AS phone, role
        )",
        user.login, user.password_hash, user.first_name, user.last_name,
        user.email, user.phone, user.role);

    if (result.IsEmpty()) {
      return std::nullopt;
    }

    return RowToUser(result[0]);
  }

  std::optional<User> FindUserByLogin(const std::string& login) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          SELECT id, login, password_hash, first_name, last_name, email,
                 COALESCE(phone, '') AS phone, role
          FROM users
          WHERE login = $1
        )",
        login);

    if (result.IsEmpty()) {
      return std::nullopt;
    }

    return RowToUser(result[0]);
  }

  std::optional<User> FindUserById(std::int64_t id) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          SELECT id, login, password_hash, first_name, last_name, email,
                 COALESCE(phone, '') AS phone, role
          FROM users
          WHERE id = $1
        )",
        id);

    if (result.IsEmpty()) {
      return std::nullopt;
    }

    return RowToUser(result[0]);
  }

  std::vector<User> SearchUsers(const std::string& mask) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          SELECT id, login, password_hash, first_name, last_name, email,
                 COALESCE(phone, '') AS phone, role
          FROM users
          WHERE LOWER(first_name) LIKE '%' || LOWER($1::text) || '%'
             OR LOWER(last_name) LIKE '%' || LOWER($1::text) || '%'
          ORDER BY id
        )",
        mask);

    std::vector<User> users;
    users.reserve(result.Size());
    for (const auto& row : result) {
      users.push_back(RowToUser(row));
    }
    return users;
  }

  std::optional<std::int64_t> AuthenticateUser(
      const std::string& login, const std::string& password) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          SELECT id
          FROM users
          WHERE login = $1 AND password_hash = $2
        )",
        login, password);

    if (result.IsEmpty()) {
      return std::nullopt;
    }

    return result.AsSingleRow<std::int64_t>();
  }

  std::optional<std::int64_t> AuthenticateToken(std::string_view token) const {
    constexpr std::string_view kPrefix = "token-";
    if (token.substr(0, kPrefix.size()) != kPrefix) {
      return std::nullopt;
    }

    std::int64_t user_id = 0;
    const auto id_view = token.substr(kPrefix.size());
    const auto* begin = id_view.data();
    const auto* end = id_view.data() + id_view.size();
    const auto [ptr, ec] = std::from_chars(begin, end, user_id);
    if (ec != std::errc{} || ptr != end || user_id <= 0) {
      return std::nullopt;
    }

    if (!FindUserById(user_id).has_value()) {
      return std::nullopt;
    }

    return user_id;
  }

  Goal CreateGoal(std::string title, std::string description,
                  std::int64_t author_id) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          INSERT INTO goals (title, description, author_id)
          VALUES ($1, $2, $3)
          RETURNING id, title, description, author_id, status
        )",
        title, description, author_id);

    return RowToGoal(result[0]);
  }

  std::vector<Goal> ListGoals() const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          SELECT id, title, description, author_id, status
          FROM goals
          ORDER BY id
        )");

    std::vector<Goal> goals;
    goals.reserve(result.Size());
    for (const auto& row : result) {
      goals.push_back(RowToGoal(row));
    }
    return goals;
  }

  std::optional<Goal> FindGoalById(std::int64_t id) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          SELECT id, title, description, author_id, status
          FROM goals
          WHERE id = $1
        )",
        id);

    if (result.IsEmpty()) {
      return std::nullopt;
    }

    return RowToGoal(result[0]);
  }

  std::optional<Task> CreateTask(std::int64_t goal_id, std::string title,
                                 std::string description,
                                 std::int64_t assignee_id,
                                 std::int64_t author_id,
                                 std::string due_date) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          INSERT INTO tasks (
              goal_id, title, description, assignee_id, author_id, due_date
          )
          VALUES ($1, $2, $3, $4, $5, NULLIF($6, '')::date)
          RETURNING id, goal_id, title, description, assignee_id, author_id,
                    status, COALESCE(due_date::text, '') AS due_date
        )",
        goal_id, title, description, assignee_id, author_id, due_date);

    if (result.IsEmpty()) {
      return std::nullopt;
    }

    return RowToTask(result[0]);
  }

  std::vector<Task> ListTasks(std::int64_t goal_id) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          SELECT id, goal_id, title, description, assignee_id, author_id,
                 status, COALESCE(due_date::text, '') AS due_date
          FROM tasks
          WHERE goal_id = $1
          ORDER BY id
        )",
        goal_id);

    std::vector<Task> tasks;
    tasks.reserve(result.Size());
    for (const auto& row : result) {
      tasks.push_back(RowToTask(row));
    }
    return tasks;
  }

  std::optional<Task> FindTask(std::int64_t goal_id,
                               std::int64_t task_id) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          SELECT id, goal_id, title, description, assignee_id, author_id,
                 status, COALESCE(due_date::text, '') AS due_date
          FROM tasks
          WHERE goal_id = $1 AND id = $2
        )",
        goal_id, task_id);

    if (result.IsEmpty()) {
      return std::nullopt;
    }

    return RowToTask(result[0]);
  }

  std::optional<Task> UpdateTaskStatus(std::int64_t goal_id,
                                       std::int64_t task_id,
                                       std::string status) const {
    const auto result = pg_cluster_->Execute(
        postgres::ClusterHostType::kMaster,
        R"(
          UPDATE tasks
          SET status = $3,
              updated_at = CURRENT_TIMESTAMP
          WHERE goal_id = $1 AND id = $2
          RETURNING id, goal_id, title, description, assignee_id, author_id,
                    status, COALESCE(due_date::text, '') AS due_date
        )",
        goal_id, task_id, status);

    if (result.IsEmpty()) {
      return std::nullopt;
    }

    return RowToTask(result[0]);
  }

 private:
  static User RowToUser(const postgres::Row& row) {
    const auto [id, login, password_hash, first_name, last_name, email, phone,
                role] =
        row.As<std::int64_t, std::string, std::string, std::string,
               std::string, std::string, std::string, std::string>();

    return User{id, login, password_hash, first_name, last_name, email, phone,
                role};
  }

  static Goal RowToGoal(const postgres::Row& row) {
    const auto [id, title, description, author_id, status] =
        row.As<std::int64_t, std::string, std::string, std::int64_t,
               std::string>();

    return Goal{id, title, description, author_id, status};
  }

  static Task RowToTask(const postgres::Row& row) {
    const auto [id, goal_id, title, description, assignee_id, author_id, status,
                due_date] =
        row.As<std::int64_t, std::int64_t, std::string, std::string,
               std::int64_t, std::int64_t, std::string, std::string>();

    return Task{id,      goal_id,     title,  description,
                assignee_id, author_id, status, due_date};
  }

  postgres::ClusterPtr pg_cluster_;
};

bson::Document ExtractDocument(bson::ValueBuilder builder) {
  return bson::Document{builder.ExtractValue()};
}

class MongoStorage final : public components::ComponentBase {
 public:
  static constexpr std::string_view kName = "mongo-storage";

  MongoStorage(const components::ComponentConfig& config,
               const components::ComponentContext& context)
      : ComponentBase(config, context),
        pool_(context.FindComponent<components::Mongo>("task-planning-mongo")
                  .GetPool()),
        task_comments_(pool_->GetCollection("task_comments")),
        task_activity_(pool_->GetCollection("task_activity")) {}

  bson::Document CreateTaskComment(std::int64_t goal_id, std::int64_t task_id,
                                   const User& author,
                                   const std::string& text,
                                   const std::vector<std::string>& tags) {
    const auto now = datetime::Now();

    bson::ValueBuilder document;
    document["_id"] = bson::Oid{};
    document["taskId"] = task_id;
    document["goalId"] = goal_id;
    document["author"]["userId"] = author.id;
    document["author"]["login"] = author.login;
    document["author"]["displayName"] =
        author.first_name + " " + author.last_name;
    document["text"] = text;
    document["tags"] = BuildTags(tags);
    document["createdAt"] = now;
    document["updatedAt"] = now;

    auto extracted = ExtractDocument(std::move(document));
    task_comments_.InsertOne(extracted);
    return extracted;
  }

  std::vector<bson::Document> ListTaskComments(std::int64_t goal_id,
                                               std::int64_t task_id) const {
    auto cursor = task_comments_.Find(
        BuildTaskFilter(goal_id, task_id),
        mongo_options::Sort{{"createdAt", mongo_options::Sort::kAscending}});

    std::vector<bson::Document> documents;
    for (const auto& document : cursor) {
      documents.push_back(document);
    }
    return documents;
  }

  void AddStatusChangedActivity(std::int64_t goal_id, std::int64_t task_id,
                                std::int64_t actor_id,
                                const std::string& new_status) {
    bson::ValueBuilder document;
    document["_id"] = bson::Oid{};
    document["taskId"] = task_id;
    document["goalId"] = goal_id;
    document["type"] = "status_changed";
    document["actor"]["userId"] = actor_id;
    document["payload"]["newStatus"] = new_status;
    document["createdAt"] = datetime::Now();

    task_activity_.InsertOne(ExtractDocument(std::move(document)));
  }

  std::vector<bson::Document> ListTaskActivity(std::int64_t goal_id,
                                               std::int64_t task_id) const {
    auto cursor = task_activity_.Find(
        BuildTaskFilter(goal_id, task_id),
        mongo_options::Sort{{"createdAt", mongo_options::Sort::kAscending}});

    std::vector<bson::Document> documents;
    for (const auto& document : cursor) {
      documents.push_back(document);
    }
    return documents;
  }

 private:
  static bson::Value BuildTags(const std::vector<std::string>& tags) {
    bson::ValueBuilder builder(formats::common::Type::kArray);
    for (const auto& tag : tags) {
      builder.PushBack(bson::ValueBuilder{tag});
    }
    return builder.ExtractValue();
  }

  static bson::Document BuildTaskFilter(std::int64_t goal_id,
                                        std::int64_t task_id) {
    bson::ValueBuilder filter;
    filter["goalId"] = goal_id;
    filter["taskId"] = task_id;
    return ExtractDocument(std::move(filter));
  }

  mongo::PoolPtr pool_;
  mongo::Collection task_comments_;
  mongo::Collection task_activity_;
};

struct ApiError {
  http::HttpStatus status;
  std::string message;
};

struct RateLimitResult {
  bool allowed{};
  std::int64_t limit{};
  std::int64_t remaining{};
  std::int64_t reset_unix_timestamp{};
};

class RedisStorage final : public components::ComponentBase {
 public:
  static constexpr std::string_view kName = "redis-storage";

  RedisStorage(const components::ComponentConfig& config,
               const components::ComponentContext& context)
      : ComponentBase(config, context),
        redis_client_(
            context.FindComponent<components::Redis>("task-planning-cache")
                .GetClient("task-planning-cache")),
        redis_cc_{std::chrono::milliseconds{100},
                  std::chrono::milliseconds{300}, 1} {}

  std::optional<std::string> GetJson(const std::string& key) const {
    try {
      return redis_client_->Get(key, redis_cc_).Get("cache get " + key);
    } catch (const redis::Exception& error) {
      LOG_WARNING() << "Redis cache get failed for key=" << key
                    << ": " << error.what();
      return std::nullopt;
    }
  }

  void SetJson(const std::string& key, const std::string& value,
               std::chrono::seconds ttl) const {
    try {
      redis_client_->Setex(key, ttl, value, redis_cc_).Get("cache set " + key);
    } catch (const redis::Exception& error) {
      LOG_WARNING() << "Redis cache set failed for key=" << key
                    << ": " << error.what();
    }
  }

  void Invalidate(const std::string& key) const {
    try {
      redis_client_->Del(key, redis_cc_).Get("cache del " + key);
    } catch (const redis::Exception& error) {
      LOG_WARNING() << "Redis cache invalidation failed for key=" << key
                    << ": " << error.what();
    }
  }

  RateLimitResult CheckUserSearchRateLimit(std::int64_t user_id) const {
    constexpr std::int64_t kLimit = 60;
    constexpr auto kWindow = std::chrono::seconds{60};

    const auto now = std::chrono::system_clock::now();
    const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 now.time_since_epoch())
                                 .count();
    const auto window_start_unix_minute = now_seconds / kWindow.count();
    const auto reset_unix_timestamp =
        (window_start_unix_minute + 1) * kWindow.count();
    const auto key = "rate:user-search:" + std::to_string(user_id) + ":" +
                     std::to_string(window_start_unix_minute);

    try {
      const auto current =
          redis_client_->Incr(key, redis_cc_).Get("rate limit incr " + key);
      if (current == 1) {
        redis_client_->Expire(key, kWindow, redis_cc_)
            .Get("rate limit expire " + key);
      }

      return RateLimitResult{current <= kLimit, kLimit,
                             std::max<std::int64_t>(0, kLimit - current),
                             reset_unix_timestamp};
    } catch (const redis::Exception& error) {
      LOG_ERROR() << "Redis rate limiter failed for key=" << key
                  << ": " << error.what();
      throw ApiError{http::HttpStatus::kServiceUnavailable,
                     "rate limiter unavailable"};
    }
  }

 private:
  redis::ClientPtr redis_client_;
  redis::CommandControl redis_cc_;
};

json::ValueBuilder BuildError(std::string_view message) {
  json::ValueBuilder builder;
  builder["error"] = std::string{message};
  return builder;
}

json::ValueBuilder BuildUser(const User& user) {
  json::ValueBuilder builder;
  builder["id"] = user.id;
  builder["login"] = user.login;
  builder["firstName"] = user.first_name;
  builder["lastName"] = user.last_name;
  builder["email"] = user.email;
  builder["phone"] = user.phone;
  builder["role"] = user.role;
  return builder;
}

json::ValueBuilder BuildGoal(const Goal& goal) {
  json::ValueBuilder builder;
  builder["id"] = goal.id;
  builder["title"] = goal.title;
  builder["description"] = goal.description;
  builder["authorId"] = goal.author_id;
  builder["status"] = goal.status;
  return builder;
}

json::ValueBuilder BuildTask(const Task& task) {
  json::ValueBuilder builder;
  builder["id"] = task.id;
  builder["goalId"] = task.goal_id;
  builder["title"] = task.title;
  builder["description"] = task.description;
  builder["assigneeId"] = task.assignee_id;
  builder["authorId"] = task.author_id;
  builder["status"] = task.status;
  builder["dueDate"] = task.due_date;
  return builder;
}

std::int64_t ReadBsonInt64(const bson::Value& value) {
  if (value.IsMissing() || value.IsNull()) {
    return 0;
  }
  if (value.IsInt64()) {
    return value.As<std::int64_t>();
  }
  if (value.IsInt32() || value.IsInt()) {
    return value.As<int>();
  }
  return value.ConvertTo<std::int64_t>();
}

std::string ReadBsonString(const bson::Value& value) {
  if (value.IsMissing() || value.IsNull()) {
    return {};
  }
  return value.As<std::string>();
}

std::string ReadBsonDateTime(const bson::Value& value) {
  if (value.IsMissing() || value.IsNull()) {
    return {};
  }
  return datetime::UtcTimestring(value.As<std::chrono::system_clock::time_point>(),
                                 datetime::kIsoFormat);
}

json::ValueBuilder BuildJsonFromBsonValue(const bson::Value& value) {
  if (value.IsMissing() || value.IsNull()) {
    return json::ValueBuilder{nullptr};
  }
  if (value.IsBool()) {
    return json::ValueBuilder{value.As<bool>()};
  }
  if (value.IsInt64()) {
    return json::ValueBuilder{value.As<std::int64_t>()};
  }
  if (value.IsInt32() || value.IsInt()) {
    return json::ValueBuilder{value.As<int>()};
  }
  if (value.IsUInt64()) {
    return json::ValueBuilder{static_cast<std::int64_t>(value.As<std::uint64_t>())};
  }
  if (value.IsDouble()) {
    return json::ValueBuilder{value.As<double>()};
  }
  if (value.IsString()) {
    return json::ValueBuilder{value.As<std::string>()};
  }
  if (value.IsDateTime()) {
    return json::ValueBuilder{ReadBsonDateTime(value)};
  }
  if (value.IsOid()) {
    return json::ValueBuilder{value.As<bson::Oid>().ToString()};
  }
  if (value.IsArray()) {
    json::ValueBuilder array(formats::common::Type::kArray);
    for (const auto& item : value) {
      array.PushBack(BuildJsonFromBsonValue(item));
    }
    return array;
  }
  if (value.IsObject()) {
    json::ValueBuilder object;
    for (auto it = value.begin(); it != value.end(); ++it) {
      object[it.GetName()] = BuildJsonFromBsonValue(*it);
    }
    return object;
  }

  return json::ValueBuilder{nullptr};
}

json::ValueBuilder BuildEmbeddedUser(const bson::Value& value) {
  json::ValueBuilder builder;
  builder["userId"] = ReadBsonInt64(value["userId"]);
  builder["login"] = ReadBsonString(value["login"]);
  builder["displayName"] = ReadBsonString(value["displayName"]);
  return builder;
}

json::ValueBuilder BuildTaskComment(const bson::Document& document) {
  json::ValueBuilder builder;
  builder["id"] = document["_id"].As<bson::Oid>().ToString();
  builder["taskId"] = ReadBsonInt64(document["taskId"]);
  builder["goalId"] = ReadBsonInt64(document["goalId"]);
  builder["author"] = BuildEmbeddedUser(document["author"]);
  builder["text"] = ReadBsonString(document["text"]);
  builder["tags"] = document["tags"].IsMissing()
                        ? json::ValueBuilder{formats::common::Type::kArray}
                        : BuildJsonFromBsonValue(document["tags"]);
  builder["createdAt"] = ReadBsonDateTime(document["createdAt"]);
  builder["updatedAt"] = ReadBsonDateTime(document["updatedAt"]);
  return builder;
}

json::ValueBuilder BuildTaskActivity(const bson::Document& document) {
  json::ValueBuilder builder;
  builder["id"] = document["_id"].As<bson::Oid>().ToString();
  builder["taskId"] = ReadBsonInt64(document["taskId"]);
  builder["goalId"] = ReadBsonInt64(document["goalId"]);
  builder["type"] = ReadBsonString(document["type"]);
  builder["actor"] = BuildEmbeddedUser(document["actor"]);
  builder["payload"] = BuildJsonFromBsonValue(document["payload"]);
  builder["visibleTo"] = document["visibleTo"].IsMissing()
                             ? json::ValueBuilder{formats::common::Type::kArray}
                             : BuildJsonFromBsonValue(document["visibleTo"]);
  builder["important"] = document["important"].IsMissing()
                             ? false
                             : document["important"].As<bool>();
  builder["createdAt"] = ReadBsonDateTime(document["createdAt"]);
  return builder;
}

std::string MakeJsonResponse(const http::HttpRequest& request,
                             http::HttpStatus status,
                             json::ValueBuilder builder) {
  request.SetResponseStatus(status);
  request.GetHttpResponse().SetHeader(std::string_view{"Content-Type"},
                                      std::string{"application/json"});
  return json::ToString(builder.ExtractValue());
}

std::string MakeJsonStringResponse(const http::HttpRequest& request,
                                   http::HttpStatus status,
                                   std::string body) {
  request.SetResponseStatus(status);
  request.GetHttpResponse().SetHeader(std::string_view{"Content-Type"},
                                      std::string{"application/json"});
  return body;
}

std::string MakeErrorResponse(const http::HttpRequest& request,
                              http::HttpStatus status,
                              std::string_view message) {
  return MakeJsonResponse(request, status, BuildError(message));
}

void SetResponseHeader(const http::HttpRequest& request, std::string_view name,
                       std::string value) {
  request.GetHttpResponse().SetHeader(name, std::move(value));
}

void SetCacheHeader(const http::HttpRequest& request, std::string_view value) {
  SetResponseHeader(request, "X-Cache", std::string{value});
}

void SetRateLimitHeaders(const http::HttpRequest& request,
                         const RateLimitResult& rate_limit) {
  SetResponseHeader(request, "X-RateLimit-Limit",
                    std::to_string(rate_limit.limit));
  SetResponseHeader(request, "X-RateLimit-Remaining",
                    std::to_string(rate_limit.remaining));
  SetResponseHeader(request, "X-RateLimit-Reset",
                    std::to_string(rate_limit.reset_unix_timestamp));
}

std::string MakeGoalTasksCacheKey(std::int64_t goal_id) {
  return "goal:" + std::to_string(goal_id) + ":tasks";
}

json::Value ParseJsonObjectBody(const http::HttpRequest& request) {
  try {
    const auto body = json::FromString(request.RequestBody());
    if (!body.IsObject()) {
      throw ApiError{http::HttpStatus::kBadRequest,
                     "request body must be a JSON object"};
    }
    return body;
  } catch (const json::Exception&) {
    throw ApiError{http::HttpStatus::kBadRequest, "invalid JSON body"};
  }
}

std::string GetRequiredString(const json::Value& body,
                              std::string_view field_name) {
  const auto value = body[std::string{field_name}];
  if (value.IsMissing() || !value.IsString()) {
    throw ApiError{http::HttpStatus::kBadRequest,
                   std::string{field_name} + " must be a non-empty string"};
  }

  auto result = value.As<std::string>();
  if (result.empty()) {
    throw ApiError{http::HttpStatus::kBadRequest,
                   std::string{field_name} + " must be a non-empty string"};
  }

  return result;
}

std::string GetOptionalString(const json::Value& body,
                              std::string_view field_name) {
  const auto value = body[std::string{field_name}];
  if (value.IsMissing()) {
    return {};
  }
  if (!value.IsString()) {
    throw ApiError{http::HttpStatus::kBadRequest,
                   std::string{field_name} + " must be a string"};
  }
  return value.As<std::string>();
}

std::string GetRequiredStringMaxLength(const json::Value& body,
                                       std::string_view field_name,
                                       std::size_t max_length) {
  auto value = GetRequiredString(body, field_name);
  if (value.size() > max_length) {
    throw ApiError{http::HttpStatus::kBadRequest,
                   std::string{field_name} + " must be at most " +
                       std::to_string(max_length) + " characters"};
  }
  return value;
}

std::vector<std::string> GetOptionalStringArray(const json::Value& body,
                                                std::string_view field_name) {
  const auto value = body[std::string{field_name}];
  if (value.IsMissing()) {
    return {};
  }
  if (!value.IsArray()) {
    throw ApiError{http::HttpStatus::kBadRequest,
                   std::string{field_name} + " must be an array of strings"};
  }

  std::vector<std::string> result;
  result.reserve(value.GetSize());
  for (std::uint32_t index = 0; index < value.GetSize(); ++index) {
    const auto item = value[index];
    if (!item.IsString()) {
      throw ApiError{http::HttpStatus::kBadRequest,
                     std::string{field_name} + " must be an array of strings"};
    }
    result.push_back(item.As<std::string>());
  }
  return result;
}

std::int64_t GetRequiredPositiveId(const json::Value& body,
                                   std::string_view field_name) {
  const auto value = body[std::string{field_name}];
  if (value.IsMissing()) {
    throw ApiError{http::HttpStatus::kBadRequest,
                   std::string{field_name} + " is required"};
  }

  try {
    const auto result = value.As<std::int64_t>();
    if (result <= 0) {
      throw ApiError{http::HttpStatus::kBadRequest,
                     std::string{field_name} + " must be positive"};
    }
    return result;
  } catch (const json::Exception&) {
    throw ApiError{http::HttpStatus::kBadRequest,
                   std::string{field_name} + " must be an integer"};
  }
}

std::int64_t ParsePositiveId(std::string_view value,
                             std::string_view field_name) {
  std::int64_t result = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(begin, end, result);
  if (ec != std::errc{} || ptr != end || result <= 0) {
    throw ApiError{http::HttpStatus::kBadRequest,
                   std::string{field_name} + " must be positive integer"};
  }
  return result;
}

bool IsValidUserRole(std::string_view role) {
  return role == "worker" || role == "manager" || role == "admin";
}

bool IsValidTaskStatus(std::string_view status) {
  return status == "new" || status == "in_progress" || status == "done" ||
         status == "cancelled";
}

bool IsValidIsoDate(std::string_view value) {
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
    return false;
  }

  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 4 || i == 7) {
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }

  return true;
}

class ApiHandlerBase : public handlers::HttpHandlerBase {
 public:
  ApiHandlerBase(const components::ComponentConfig& config,
                 const components::ComponentContext& context)
      : HttpHandlerBase(config, context),
        storage_(context.FindComponent<PostgresStorage>()),
        mongo_storage_(context.FindComponent<MongoStorage>()),
        redis_storage_(context.FindComponent<RedisStorage>()) {}

  std::string HandleRequestThrow(
      const http::HttpRequest& request,
      server::request::RequestContext& context) const final {
    try {
      return HandleApiRequest(request, context);
    } catch (const ApiError& error) {
      return MakeErrorResponse(request, error.status, error.message);
    }
  }

 protected:
  virtual std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext& context) const = 0;

  std::int64_t RequireAuth(const http::HttpRequest& request) const {
    constexpr std::string_view kPrefix = "Bearer ";

    const auto& auth_header = request.GetHeader("Authorization");
    const auto auth_view = std::string_view{auth_header};
    if (auth_view.substr(0, kPrefix.size()) != kPrefix) {
      throw ApiError{http::HttpStatus::kUnauthorized,
                     "missing or invalid Authorization header"};
    }

    const auto user_id =
        storage_.AuthenticateToken(auth_view.substr(kPrefix.size()));
    if (!user_id.has_value()) {
      throw ApiError{http::HttpStatus::kUnauthorized, "invalid bearer token"};
    }

    return *user_id;
  }

  PostgresStorage& storage_;
  MongoStorage& mongo_storage_;
  RedisStorage& redis_storage_;
};

class PingHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-ping";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    json::ValueBuilder builder;
    builder["status"] = "ok";
    return MakeJsonResponse(request, http::HttpStatus::kOk, std::move(builder));
  }
};

class UsersHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-users";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    const auto body = ParseJsonObjectBody(request);

    User user;
    user.login = GetRequiredString(body, "login");
    user.password_hash = GetRequiredString(body, "password");
    user.first_name = GetRequiredString(body, "firstName");
    user.last_name = GetRequiredString(body, "lastName");
    user.email = GetRequiredString(body, "email");
    user.phone = GetOptionalString(body, "phone");
    user.role = GetRequiredString(body, "role");

    if (!IsValidUserRole(user.role)) {
      throw ApiError{http::HttpStatus::kBadRequest, "invalid user role"};
    }

    const auto created = storage_.CreateUser(user);
    if (!created.has_value()) {
      throw ApiError{http::HttpStatus::kConflict,
                     "login or email already exists"};
    }

    return MakeJsonResponse(request, http::HttpStatus::kCreated,
                            BuildUser(*created));
  }
};

class LoginHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-login";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    const auto body = ParseJsonObjectBody(request);
    const auto login = GetRequiredString(body, "login");
    const auto password = GetRequiredString(body, "password");

    const auto user_id = storage_.AuthenticateUser(login, password);
    if (!user_id.has_value()) {
      throw ApiError{http::HttpStatus::kUnauthorized,
                     "invalid login or password"};
    }

    json::ValueBuilder builder;
    builder["token"] = "token-" + std::to_string(*user_id);
    return MakeJsonResponse(request, http::HttpStatus::kOk, std::move(builder));
  }
};

class UserByLoginHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-user-by-login";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    RequireAuth(request);

    const auto& login = request.GetArg("login");
    if (login.empty()) {
      throw ApiError{http::HttpStatus::kBadRequest,
                     "login query parameter is required"};
    }

    const auto user = storage_.FindUserByLogin(login);
    if (!user.has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "user not found"};
    }

    return MakeJsonResponse(request, http::HttpStatus::kOk, BuildUser(*user));
  }
};

class UserSearchHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-user-search";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    const auto user_id = RequireAuth(request);
    const auto rate_limit = redis_storage_.CheckUserSearchRateLimit(user_id);
    SetRateLimitHeaders(request, rate_limit);
    if (!rate_limit.allowed) {
      return MakeErrorResponse(request, http::HttpStatus::kTooManyRequests,
                               "rate limit exceeded");
    }

    const auto& mask = request.GetArg("mask");
    if (mask.empty()) {
      throw ApiError{http::HttpStatus::kBadRequest,
                     "mask query parameter is required"};
    }

    json::ValueBuilder users(formats::common::Type::kArray);
    for (const auto& user : storage_.SearchUsers(mask)) {
      users.PushBack(BuildUser(user));
    }

    return MakeJsonResponse(request, http::HttpStatus::kOk, std::move(users));
  }
};

class GoalsHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-goals";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    const auto author_id = RequireAuth(request);

    if (request.GetMethod() == http::HttpMethod::kGet) {
      constexpr std::string_view kCacheKey = "goals:all";
      if (const auto cached = redis_storage_.GetJson(std::string{kCacheKey})) {
        SetCacheHeader(request, "HIT");
        return MakeJsonStringResponse(request, http::HttpStatus::kOk, *cached);
      }

      json::ValueBuilder goals(formats::common::Type::kArray);
      for (const auto& goal : storage_.ListGoals()) {
        goals.PushBack(BuildGoal(goal));
      }

      auto body = json::ToString(goals.ExtractValue());
      redis_storage_.SetJson(std::string{kCacheKey}, body,
                             std::chrono::seconds{60});
      SetCacheHeader(request, "MISS");
      return MakeJsonStringResponse(request, http::HttpStatus::kOk,
                                    std::move(body));
    }

    const auto body = ParseJsonObjectBody(request);
    const auto title = GetRequiredString(body, "title");
    const auto description = GetRequiredString(body, "description");
    const auto goal = storage_.CreateGoal(title, description, author_id);
    redis_storage_.Invalidate("goals:all");

    return MakeJsonResponse(request, http::HttpStatus::kCreated,
                            BuildGoal(goal));
  }
};

class GoalTasksHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-goal-tasks";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    const auto author_id = RequireAuth(request);
    const auto goal_id = ParsePositiveId(request.GetPathArg("goalId"), "goalId");

    if (request.GetMethod() == http::HttpMethod::kGet) {
      const auto cache_key = MakeGoalTasksCacheKey(goal_id);
      if (const auto cached = redis_storage_.GetJson(cache_key)) {
        SetCacheHeader(request, "HIT");
        return MakeJsonStringResponse(request, http::HttpStatus::kOk, *cached);
      }

      if (!storage_.FindGoalById(goal_id).has_value()) {
        throw ApiError{http::HttpStatus::kNotFound, "goal not found"};
      }

      json::ValueBuilder tasks(formats::common::Type::kArray);
      for (const auto& task : storage_.ListTasks(goal_id)) {
        tasks.PushBack(BuildTask(task));
      }

      auto body = json::ToString(tasks.ExtractValue());
      redis_storage_.SetJson(cache_key, body, std::chrono::seconds{30});
      SetCacheHeader(request, "MISS");
      return MakeJsonStringResponse(request, http::HttpStatus::kOk,
                                    std::move(body));
    }

    if (!storage_.FindGoalById(goal_id).has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "goal not found"};
    }

    const auto body = ParseJsonObjectBody(request);
    const auto title = GetRequiredString(body, "title");
    const auto description = GetRequiredString(body, "description");
    const auto assignee_id = GetRequiredPositiveId(body, "assigneeId");
    const auto due_date = GetOptionalString(body, "dueDate");

    if (!due_date.empty() && !IsValidIsoDate(due_date)) {
      throw ApiError{http::HttpStatus::kBadRequest,
                     "dueDate must have YYYY-MM-DD format"};
    }

    if (!storage_.FindUserById(assignee_id).has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "assignee user not found"};
    }

    const auto task = storage_.CreateTask(goal_id, title, description,
                                          assignee_id, author_id, due_date);
    if (!task.has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "goal or assignee not found"};
    }
    redis_storage_.Invalidate(MakeGoalTasksCacheKey(goal_id));

    return MakeJsonResponse(request, http::HttpStatus::kCreated,
                            BuildTask(*task));
  }
};

class TaskStatusHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-task-status";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    const auto actor_id = RequireAuth(request);

    const auto goal_id = ParsePositiveId(request.GetPathArg("goalId"), "goalId");
    const auto task_id = ParsePositiveId(request.GetPathArg("taskId"), "taskId");

    if (!storage_.FindGoalById(goal_id).has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "goal not found"};
    }

    const auto body = ParseJsonObjectBody(request);
    const auto status = GetRequiredString(body, "status");
    if (!IsValidTaskStatus(status)) {
      throw ApiError{http::HttpStatus::kBadRequest, "invalid task status"};
    }

    const auto task = storage_.UpdateTaskStatus(goal_id, task_id, status);
    if (!task.has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "task not found"};
    }

    redis_storage_.Invalidate(MakeGoalTasksCacheKey(goal_id));
    mongo_storage_.AddStatusChangedActivity(goal_id, task_id, actor_id, status);

    return MakeJsonResponse(request, http::HttpStatus::kOk, BuildTask(*task));
  }
};

class TaskCommentsHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-task-comments";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    const auto author_id = RequireAuth(request);
    const auto goal_id = ParsePositiveId(request.GetPathArg("goalId"), "goalId");
    const auto task_id = ParsePositiveId(request.GetPathArg("taskId"), "taskId");

    if (!storage_.FindGoalById(goal_id).has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "goal not found"};
    }
    if (!storage_.FindTask(goal_id, task_id).has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "task not found"};
    }

    if (request.GetMethod() == http::HttpMethod::kGet) {
      json::ValueBuilder comments(formats::common::Type::kArray);
      for (const auto& comment :
           mongo_storage_.ListTaskComments(goal_id, task_id)) {
        comments.PushBack(BuildTaskComment(comment));
      }
      return MakeJsonResponse(request, http::HttpStatus::kOk,
                              std::move(comments));
    }

    const auto body = ParseJsonObjectBody(request);
    const auto text = GetRequiredStringMaxLength(body, "text", 2000);
    const auto tags = GetOptionalStringArray(body, "tags");

    const auto author = storage_.FindUserById(author_id);
    if (!author.has_value()) {
      throw ApiError{http::HttpStatus::kUnauthorized, "invalid bearer token"};
    }

    const auto comment = mongo_storage_.CreateTaskComment(
        goal_id, task_id, *author, text, tags);
    return MakeJsonResponse(request, http::HttpStatus::kCreated,
                            BuildTaskComment(comment));
  }
};

class TaskActivityHandler final : public ApiHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-task-activity";
  using ApiHandlerBase::ApiHandlerBase;

 private:
  std::string HandleApiRequest(
      const http::HttpRequest& request,
      server::request::RequestContext&) const override {
    RequireAuth(request);

    const auto goal_id = ParsePositiveId(request.GetPathArg("goalId"), "goalId");
    const auto task_id = ParsePositiveId(request.GetPathArg("taskId"), "taskId");

    if (!storage_.FindGoalById(goal_id).has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "goal not found"};
    }
    if (!storage_.FindTask(goal_id, task_id).has_value()) {
      throw ApiError{http::HttpStatus::kNotFound, "task not found"};
    }

    json::ValueBuilder activity(formats::common::Type::kArray);
    for (const auto& event : mongo_storage_.ListTaskActivity(goal_id, task_id)) {
      activity.PushBack(BuildTaskActivity(event));
    }
    return MakeJsonResponse(request, http::HttpStatus::kOk,
                            std::move(activity));
  }
};

}  // namespace task_planning

int main(int argc, char* argv[]) {
  const auto component_list =
      userver::components::MinimalServerComponentList()
          .Append<userver::components::TestsuiteSupport>()
          .Append<userver::clients::dns::Component>()
          .Append<userver::components::Secdist>()
          .Append<userver::components::DefaultSecdistProvider>()
          .Append<userver::components::Postgres>("task-planning-db")
          .Append<userver::components::Mongo>("task-planning-mongo")
          .Append<userver::components::Redis>("task-planning-cache")
          .Append<task_planning::PostgresStorage>()
          .Append<task_planning::MongoStorage>()
          .Append<task_planning::RedisStorage>()
          .Append<task_planning::PingHandler>()
          .Append<task_planning::UsersHandler>()
          .Append<task_planning::LoginHandler>()
          .Append<task_planning::UserByLoginHandler>()
          .Append<task_planning::UserSearchHandler>()
          .Append<task_planning::GoalsHandler>()
          .Append<task_planning::GoalTasksHandler>()
          .Append<task_planning::TaskStatusHandler>()
          .Append<task_planning::TaskCommentsHandler>()
          .Append<task_planning::TaskActivityHandler>();

  return userver::utils::DaemonMain(argc, argv, component_list);
}
