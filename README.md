# App Template

A C++26 web application starter built with Drogon and Xmake. The home page is a social feed with cookie-based sign-in and SQLite storage. The server also exposes file upload, download, and upstream content endpoints.

## Getting started

Install Xmake for your platform.

**macOS** with Homebrew

```sh
brew install xmake
```

**Linux** with the official installer

```sh
curl -fsSL https://xmake.io/shget.text | bash
```

Open a new shell after installation and run `xmake --version`. For package-manager alternatives, see the [Xmake installation guide](https://xmake.io/guide/quick-start.html#installation).

Use a C++26 compiler and standard library with `std::println` support. The first build needs network access or a populated Xmake package cache. From the project root, run:

```sh
xmake f -m debug
xmake
xmake run app_server
```

Open <http://127.0.0.1:8080> and sign in with the local demo credentials `test_user` / `test_password`. Run the test suite from the project root:

```sh
xmake test
```

## Project layout

| Path | Contents |
| --- | --- |
| `src/framework/` | Reusable configuration, database, CRUD, HTTP, JWT, upload storage, logging, and worker code |
| `src/main.cpp` | Application entry point |
| `src/app/` | Demo application organized into server, services, DAO, and model layers |
| `src/app/config.h` | Application settings, defaults, and validation |
| `views/` | Drogon CSP templates compiled by Xmake |
| `public/` | Browser JavaScript, Tailwind source and generated CSS, and runtime uploads |
| `tests/` | Catch2 tests |

`template_core` builds the reusable static library. `app_core` builds the demo
features and links to it. Run `xmake build template_core` to build the shared
library on its own.

To add a feature, put its HTTP handlers in `src/app/server/routes/`, business
logic in `src/app/services/`, and persistence in `src/app/dao/`. Put persisted
records and DTOs in separate headers under `src/app/models/`. Use the public
framework headers for persistence, CRUD, or HTTP responses. Wire the feature
into the service container and route registration in `src/app/server/`.

`src/app/server/server.h` contains `serve()` and its inline startup helpers
for Drogon setup and lifecycle callbacks. Startup calls `register_app_routes()`
in `src/app/server/routes.cpp` to register JWT middleware and mount the
feature modules. Each module receives the Drogon server explicitly and lists
its paths, methods, and middleware with `registerHandler()`. Put each HTTP
method on its own route registration and keep the handler beside the route list.
The framework provides separate HTTP hooks for access logging and exception handling.

## Database example

The database layer opens one shared SQLite backend through Drogon. Each DAO maps a C++ record to a table and provides coroutine-based queries. In the server, startup initializes the DAOs, services own them and handle business logic, and routes call the services.

### Define records and DAOs

Use a fields struct with a `table_name`, wrap it in `DaoRecord`, and derive a DAO from `BaseDao`. Field names become column names. `DaoRecord` supplies a generated integer ID unless a field declares `Primary<T>`. Timestamps are enabled by default. Each DAO can opt out by passing `DaoOptions{.timestamps = false}` as the third template argument to `BaseDao`.

```cpp
#include "framework/db/base_dao.h"

#include <print>
#include <string>
#include <string_view>
#include <utility>

namespace APP_NAMESPACE {

struct ProjectFields {
    static constexpr std::string_view table_name = "projects";

    Unique<std::string> name;
};
using ProjectRecord = DaoRecord<ProjectFields>;

class ProjectDao final : public BaseDao<ProjectDao, ProjectRecord> {
public:
    static drogon::Task<ProjectDao> init(DatabaseBackendPtr database) {
        return finish_initialization(ProjectDao(std::move(database)));
    }

private:
    explicit ProjectDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}
};

enum class TaskStatus { draft, done };

struct TaskFields {
    static constexpr std::string_view table_name = "tasks";

    std::string title;
    Indexed<RequiredForeign<ProjectRecord>> project_id;
    Foreign<ProjectRecord> related_project_id;
    Default<TaskStatus, TaskStatus::draft> status;
    std::optional<std::string> note;
    std::optional<DaoDateTime> due_at;
};
using TaskRecord = DaoRecord<TaskFields>;

class TaskDao final : public BaseDao<TaskDao, TaskRecord> {
public:
    static drogon::Task<TaskDao> init(DatabaseBackendPtr database) {
        return finish_initialization(TaskDao(std::move(database)));
    }

private:
    explicit TaskDao(DatabaseBackendPtr database) : BaseDao(std::move(database)) {}
};

}
```

`Foreign<T>` is nullable by default. Use `RequiredForeign<T>` for a required reference. Referenced rows are protected from deletion while a child points to them. Plain enums need no registration. Use distinct values in the range `-128` to `127`, and treat enumerator names as stored labels. `DaoDateTime` represents a UTC time point with millisecond precision.

### Connect and run queries

Place this coroutine after the definitions above. It uses an in-memory database so you can run it repeatedly. For persistence, pass a path such as `".local/example.sqlite3"` after creating the parent directory. Initialize referenced tables first.

```cpp
namespace APP_NAMESPACE {

drogon::Task<> databaseExample() {
    auto database = co_await open_sqlite_database(":memory:");
    auto projects = co_await ProjectDao::init(database);
    auto tasks = co_await TaskDao::init(database);

    // Insert rows and get their IDs.
    const auto website_id = co_await projects.create({{"Website"}});
    const auto docs_id = co_await projects.create({{"Documentation"}});
    TaskRecord draft{{"Write docs", website_id, docs_id}};
    draft.note = "Start with the database example.";
    const auto task_id = co_await tasks.create(draft);

    // Filter, select optional fields, sort, and fetch the first page.
    auto page = co_await tasks.find()
        .limit(20)
        .select<&TaskRecord::note, &TaskRecord::related_project_id>()
        .order_by<&TaskRecord::title>()
        .where<&TaskRecord::project_id>(website_id)
        .collect();

    if (!page.empty()) {
        auto& task = page.front();
        if (task.is_loaded<&TaskRecord::note>() && task.note) {
            std::println("Note: {}", *task.note);
        }
        task.status.value = TaskStatus::done;
        co_await tasks.update(task);
    }

    // Count matches across all pages.
    const auto total = co_await tasks.count()
        .where<&TaskRecord::project_id>(in({website_id, docs_id}))
        .where<&TaskRecord::title>(contains("docs"))
        .collect();
    std::println("Matching tasks: {}", total);

    // Read by ID and explicitly fetch the related project.
    const auto task = co_await tasks.find_by_id(task_id)
        .with_foreign<&TaskRecord::related_project_id>(projects)
        .collect();
    if (task && task->related_project_id.record()) {
        std::println("Related project: {}", task->related_project_id.record()->name.value);
    }

    // Run custom SQL with a bound parameter.
    const auto rows = co_await tasks.execute_sql("SELECT title FROM tasks WHERE id = ?", task_id);
    if (!rows.empty()) std::println("Title: {}", rows[0]["title"].as<std::string>());

    co_await tasks.delete_by_id(task_id);
}

}
```

### Operation reference

| Operation | Behavior |
| --- | --- |
| `init(database)` | Creates or validates the table and applies supported additive schema upgrades. |
| `create(record)` | Inserts a row and returns its primary key. Leave generated IDs empty on insert. |
| `find().collect()` | Defaults to 100 rows at offset 0. Returns the key and required fields, sorted by newest update when timestamps are enabled and by primary key otherwise. Use `.select<...>()` or `.select_all()` for nullable fields, and `.order_by<Member>(SortDirection::desc)` for descending order. |
| `find_by_id(id).collect()` | Returns an optional full record. Chain `.with_foreign<Member>(target_dao)` to load selected related records. |
| `update(record)` | Updates by primary key and preserves fields that the query omitted. A loaded nullable field set to `std::nullopt` clears the value. |
| `update_by_id(id).set<Member>(value).execute()` | Updates only assigned fields. Use `std::nullopt` to clear a nullable field. Returns false for missing and soft-deleted rows. |
| `create_many(records)` | Inserts the batch atomically and returns keys in input order. |
| `update_many(records)` | Updates the batch atomically. Returns false and rolls back if a row is missing or soft-deleted. |
| `delete_by_id(id)` | Deletes by primary key and reports whether a row was deleted. |
| `delete_many(ids)`, `soft_delete_many(ids)` | Delete by primary key atomically and return the number of rows changed. |
| `insert_if_absent<Key>(record)`, `upsert<Key, Members...>(record)` | Handle conflicts on a declared unique field or explicit primary key. Insert-if-absent returns whether it inserted. Upsert updates only the listed fields on conflict and returns the row's key. |
| `count().where<Member>(value).collect()` | Counts matches across all pages. Omit filters to count all active rows. |
| `execute_sql(sql, args...)` | Runs custom, backend-specific SQL with bound arguments. |

`.where<Member>(value)` means equality. Both `find()` and `count()` support
`.where_gt`, `.where_gte`, `.where_lt`, `.where_lte`, `.where_in`, and
`.where_contains`. For example, use `.where_in<&TaskRecord::id>({1, 2})` for
membership. Separate filters combine with `AND`. Contains matches literal,
case-sensitive text. Pass a reusable or negated predicate to `.where(predicate)`.

### Declare a unique pair

For a many-to-many table, use `CompositeUnique` to prevent duplicates without replacing the generated ID.

```cpp
#include "app/dao/user_dao.h"

namespace APP_NAMESPACE {

struct ProjectMemberFields {
    static constexpr std::string_view table_name = "project_members";

    RequiredForeign<ProjectRecord> project_id;
    RequiredForeign<UserRecord> user_id;

    static constexpr auto constraints = CompositeUnique<
        &ProjectMemberFields::project_id,
        &ProjectMemberFields::user_id>{};
};
using ProjectMemberRecord = DaoRecord<ProjectMemberFields>;

}
```

Initialize the project and user DAOs first. The membership DAO creates or reuses a unique index on `(project_id, user_id)`. Each project can have many users, and each user can join many projects. Use a `std::tuple` of descriptors when a table needs multiple composite constraints.

List queries can load multiple direct foreign fields. Pass each selection alongside
the usual pagination, filters, and ordering:

```cpp
auto page = co_await posts.find()
    .limit(20)
    .with_foreign<&PostRecord::author_id>(users)
    .with_foreign<&PostRecord::reviewer_id>(reviewers)
    .collect();
```

The DAO loads targets in batches for each selected field. Access a loaded
target through `record()` on its foreign field. Null keys and missing targets
leave `record()` empty. Use `find(tx)` to read the page and its targets in
the same transaction. Finish the builder with `.collect()` and await the result.

### Soft-delete and restore records

Every DAO table has a managed, nullable `deleted_at` column. Existing rows
start active. Use `soft_delete_by_id(id)` to mark a row deleted and
`restore_by_id(id)` to make it active again. Both accept a transaction as their
first argument and return whether the row changed state. Timestamp-enabled
tables refresh `updated_at` for both operations. `delete_by_id(id)` physically
removes either an active or a deleted row.

Normal reads, counts, and foreign loading use active rows. Chain
`.include_deleted()` on a `find()`, `find_by_id(id)`, or `count()` builder to
include deleted rows for that query. Foreign targets still use active rows. Use
`.select<&Record::deleted_at>()` or `.select_all()`
to read the timestamp in a list query. Deleted rows retain their foreign
references and unique values until physically removed.

### Run a transaction

Pass an existing transaction as the first argument to a bulk write to run the
batch inside a savepoint. A failed batch rolls back its own changes. Validation
and database errors propagate to the caller. Keep the DAO and any borrowed
transaction alive until the awaited operation completes.

Pass the transaction context to every DAO call inside the callback. The callback can use typed operations and bound SQL across DAOs on the same backend.

```cpp
co_await database->transaction([&](DaoTransaction& tx) -> drogon::Task<> {
    const auto project_id = co_await projects.create(tx, {{"Website"}});
    co_await tasks.create(tx, {{"Write docs", project_id}});
});
```

The backend commits when the callback completes and rolls back if it throws. Use the context only inside the callback. On SQLite the callback holds the shared connection, so awaiting an ordinary DAO call that omits `tx` would wait for that same connection. Let the callback manage transaction boundaries, and use `execute_sql(tx, sql, args...)` for custom queries within it.

## Configuration

Set defaults in `config.json`. Every top-level config field can be set through
the process environment or `.env` using `APP_` followed by the uppercase JSON key.
For example, `APP_LOG_LEVEL=warn` sets `log_level`. The precedence is process
environment, then `.env`, then JSON, then C++ defaults. Environment-provided fields
can be omitted from the JSON file. See [config.json](config.json) and [`.env.example`](.env.example).

Keep sensitive values in the process environment or in the ignored `.env` file.
For example, set `APP_TEST_PASSWORD` there and omit `test_password` from the JSON.
Use `APP_JWT_SECRET` to keep sessions valid across restarts. The shipped demo
credentials are for local testing.

Use one `KEY=VALUE` per line in `.env`. Blank lines and lines beginning with
`#` are ignored. Outer single or double quotes preserve whitespace and special
characters. Dollar signs are literal. String fields use the value as text.
Other fields use JSON values such as `true`, `3600`, or `["one","two"]`.

The default C++ namespace is `App`. Set a different one with `xmake f --app_namespace=MyApp`.

Define all settings and defaults as public members of a plain struct in
`src/app/config.h`. Each member name supplies the JSON key and the corresponding
environment variable. For example, `site_name` maps to `APP_SITE_NAME`.

```cpp
#include "app_namespace.h"

#include <string>

namespace APP_NAMESPACE {

struct Config {
    std::string site_name = "My app";
};

}
```

Include `framework/support/base_config.h` to call `load_config<Config>(root)` at
startup. After applying environment values, the loader calls the config type's
optional public `void validate() const`.

## Main dependencies

- **Drogon** for HTTP routing, async outbound requests, and server-rendered views
- **SQLite, libsodium, and jwt-cpp** for account storage, password hashing, and signed sessions
- **simdjson and Glaze** for JSON handling
- **Tailwind CSS** for styling and **Catch2** for tests

## Development rules

- Keep `.h` files beside their `.cpp` implementations. Use four spaces and put opening braces on the declaration line.
- Use `snake_case` for project-owned C++ functions, methods, variables, fields, constants, and enum values. Use `PascalCase` for types. Keep macro names, third-party API names, and existing JSON, configuration, and database names stable.
- Keep route handlers small. Delegate persistence and outbound calls, use `co_await` for asynchronous work, `api_ok` and `api_error` for typed JSON replies, and `std::println` for console output.
- After editing a view or Tailwind source, run `npm ci` and `npm run build:css` with Node.js installed. Include the regenerated `public/demo.css` in the change.
- Keep `.env`, local databases, and runtime uploads out of commits. See [AGENTS.md](AGENTS.md) for the full repository guidelines.

## CRUD services and routes

Use `CrudService<Dao, Resource>` for resources with a standard lifecycle. Give
the service an initialized DAO and mount its router during server startup.

### Mount a todo router

Assume `TodoDao` stores `title` and `done`, and `TodoResource` defines the
public JSON fields and allows filtering by `done`. In your startup coroutine,
mount the routes before calling `server.run()`.

```cpp
using TodoService = CrudService<TodoDao, TodoResource>;
using TodoRouter = CrudRouter<TodoService>;

auto dao = co_await TodoDao::init(database);
auto service = std::make_shared<TodoService>(std::move(dao));
auto router = std::make_shared<TodoRouter>(service);

TodoRouter::mount(server, "/api/todos", router);
```

| Request | Behavior |
| --- | --- |
| `POST /api/todos` with `{"title":"Buy milk"}` | Creates a todo and returns it with `201`. |
| `GET /api/todos?done=false&per_page=10` | Lists up to 10 open todos. |
| `GET /api/todos/1` | Fetches todo `1`. |
| `PATCH /api/todos/1` with `{"done":true}` | Marks it complete and preserves the title. |
| `DELETE /api/todos/1` | Deletes the todo and returns an empty data array. |

A successful PATCH returns a single item in the data array.

```json
{
  "success": true,
  "data": [{"id": 1, "title": "Buy milk", "done": true}],
  "meta": {
    "timestamp": "2026-11-25T00:13:00Z",
    "request_id": "req_98abc",
    "pagination": {"page": 1, "per_page": 1, "has_more": false}
  }
}
```

Pass `CrudMethods{.remove = false}` as the router's second constructor argument
to disable deletion. A disabled method returns `405` with an `Allow` header.
Pass middleware names as the fourth argument to `mount` to protect the routes.
List requests use one-based `page` and `per_page` (default 20, maximum 100).
`has_more` comes from fetching one extra matching row. Responses have no total
count. Single-item and empty DELETE responses use page and page size 1.

Use `api_ok(*request, item)` for success and
`api_error(*request, status, code, message)` for explicit errors. Errors include
`success: false`, a structured `error`, and request metadata. Throw
`RequestValidationError(message, fields)` to automatically return
`400 VALIDATION_FAILED`. The optional field map stores a list of messages per
field. The response's `X-Request-ID` header matches `meta.request_id`.

### Define and customize the resource

`TodoResource` supplies the public API policy through these DTOs and static functions.

| Policy member | Purpose |
| --- | --- |
| `Response` and `to_response(const TodoRecord&)` | Return fields clients may read in a Glaze-serializable DTO. |
| `Create`, `parse_create(const Json::Value&)`, and `make_record(Create)` | Validate a POST object, reject unknown fields, and build the new record. |
| `Patch`, `parse_patch(const Json::Value&)`, and `apply_patch(TodoRecord&, Patch)` | Validate writable fields and apply only supplied changes. |
| `add_filter(TodoDao::EqualityFilters&, std::string_view, std::string)` | Map each allowed query key to a typed equality filter and reject unknown keys. |
| Optional `parse_id(std::string_view)` | Validate resource-specific ID formats. Generated integer IDs use positive decimal values by default. |

For example, after validating that `done` is `true` or `false`, use
`filters.equal<&TodoFields::done>(value == "true")`. Throw
`CrudValidationError` for invalid client input. For a nullable PATCH field, an
outer optional can track whether the client supplied the field and an inner
optional can represent its value or explicit null. Leave IDs and timestamps out
of the writable PATCH mapping. See [the widget policy](tests/crud_tests.cpp)
for a complete example.

Derive from the service and override `list`, `get`, `create`, `patch`, or
`remove` for business rules. Derive from the router to override the same
operation hooks when behavior needs access to the HTTP request. Keep response
formatting and method restrictions in the shared dispatcher.

## Social feed demo

Open `/` and sign in with the local demo account, or create an account
on the sign-in page. Publish a post, then sign out and create or sign in to a
second account. Open the post's comments, add a comment, and like the post.
Sign back in as the first user to see the second user's comment and the shared
like count. Your own like state is tracked separately. Posts, comments, and likes
are stored in the local SQLite database.
