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
| `src/server/` | Drogon startup, feature routes, authentication middleware, and JSON responses |
| `src/services/`, `src/db/` | Account logic, upload storage, upstream HTTP calls, and SQLite persistence |
| `src/utils/` | Configuration, logging, random IDs, and worker queue |
| `views/` | Drogon CSP templates compiled by Xmake |
| `public/` | Browser JavaScript, Tailwind source and generated CSS, and runtime uploads |
| `tests/` | Catch2 tests |

## Database example

The database layer opens one shared SQLite backend through Drogon. Each DAO maps a C++ record to a table and provides coroutine-based queries. In the server, startup initializes the DAOs, services own them and handle business logic, and routes call the services.

### Define records and DAOs

Use a fields struct with a `table_name`, wrap it in `DaoRecord`, and derive a DAO from `BaseDao`. Field names become column names. `DaoRecord` supplies a generated integer ID unless a field declares `Primary<T>`. Timestamps are enabled by default. Each DAO can opt out by passing `DaoOptions{.timestamps = false}` as the third template argument to `BaseDao`.

```cpp
#include "db/base_dao.hpp"

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
        return finishInitialization(ProjectDao(std::move(database)));
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
        return finishInitialization(TaskDao(std::move(database)));
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
    auto database = co_await openSqliteDatabase(":memory:");
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
        .orderBy<&TaskRecord::title>()
        .where<&TaskRecord::project_id>(website_id)
        .collect();

    if (!page.empty()) {
        auto& task = page.front();
        if (task.isLoaded<&TaskRecord::note>() && task.note) {
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
    const auto task = co_await tasks.findById(task_id)
        .withForeign<&TaskRecord::related_project_id>(projects)
        .collect();
    if (task && task->related_project_id.record()) {
        std::println("Related project: {}", task->related_project_id.record()->name.value);
    }

    // Run custom SQL with a bound parameter.
    const auto rows = co_await tasks.executeSql("SELECT title FROM tasks WHERE id = ?", task_id);
    if (!rows.empty()) std::println("Title: {}", rows[0]["title"].as<std::string>());

    co_await tasks.deleteById(task_id);
}

}
```

### Operation reference

| Operation | Behavior |
| --- | --- |
| `init(database)` | Creates or validates the table and applies supported additive schema upgrades. |
| `create(record)` | Inserts a row and returns its primary key. Leave generated IDs empty on insert. |
| `find().collect()` | Defaults to 100 rows at offset 0. Returns the key and required fields, sorted by newest update when timestamps are enabled and by primary key otherwise. Use `.select<...>()` or `.selectAll()` for nullable fields, and `.orderBy<Member>(SortDirection::desc)` for descending order. |
| `findById(id).collect()` | Returns an optional full record. Chain `.withForeign<Member>(target_dao)` to load selected related records. |
| `update(record)` | Updates by primary key and preserves fields that the query omitted. A loaded nullable field set to `std::nullopt` clears the value. |
| `createMany(records)` | Inserts the batch atomically and returns keys in input order. |
| `updateMany(records)` | Updates the batch atomically. Returns false and rolls back if a row is missing or soft-deleted. |
| `deleteById(id)` | Deletes by primary key and reports whether a row was deleted. |
| `count().where<Member>(value).collect()` | Counts matches across all pages. Omit filters to count all active rows. |
| `executeSql(sql, args...)` | Runs custom, backend-specific SQL with bound arguments. |

`where<Member>(value)` means equality. Use `eq`, `gt`, `gte`, `lt`, `lte`, `in`, or `contains` to name an operator. Separate filters combine with `AND`. `contains` matches literal, case-sensitive text. `negate(where<...>(...))` includes rows with `NULL` when the original filter excludes them.

### Declare a unique pair

For a many-to-many table, use `CompositeUnique` to prevent duplicates without replacing the generated ID.

```cpp
#include "db/user_dao.hpp"

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
    .withForeign<&PostRecord::author_id>(users)
    .withForeign<&PostRecord::reviewer_id>(reviewers)
    .collect();
```

The DAO loads targets in batches for each selected field. Access a loaded
target through `record()` on its foreign field. Null keys and missing targets
leave `record()` empty. Use `find(tx)` to read the page and its targets in
the same transaction. Finish the builder with `.collect()` and await the result.

### Soft-delete and restore records

Every DAO table has a managed, nullable `deleted_at` column. Existing rows
start active. Use `softDeleteById(id)` to mark a row deleted and
`restoreById(id)` to make it active again. Both accept a transaction as their
first argument and return whether the row changed state. Timestamp-enabled
tables refresh `updated_at` for both operations. `deleteById(id)` physically
removes either an active or a deleted row.

Normal reads, counts, and foreign loading use active rows. Chain
`.includeDeleted()` on a `find()`, `findById(id)`, or `count()` builder to
include deleted rows for that query. Foreign targets still use active rows. Use
`.select<&Record::deleted_at>()` or `.selectAll()`
to read the timestamp in a list query. Deleted rows retain their foreign
references and unique values until physically removed.

### Run a transaction

Pass an existing transaction as the first argument to `createMany` or
`updateMany` to run the batch inside a savepoint. A failed batch rolls back
its own changes. Validation and database errors propagate to the caller.

Pass the transaction context to every DAO call inside the callback. The callback can use typed operations and bound SQL across DAOs on the same backend.

```cpp
co_await database->transaction([&](DaoTransaction& tx) -> drogon::Task<> {
    const auto project_id = co_await projects.create(tx, {{"Website"}});
    co_await tasks.create(tx, {{"Write docs", project_id}});
});
```

The backend commits when the callback completes and rolls back if it throws. Use the context only inside the callback. On SQLite the callback holds the shared connection, so awaiting an ordinary DAO call that omits `tx` would wait for that same connection. Let the callback manage transaction boundaries, and use `executeSql(tx, sql, args...)` for custom queries within it.

## Configuration

Edit [config.json](config.json) to change the listener or demo credentials. The shipped credentials are for local testing. [`.env.example`](.env.example) lists the optional `APP_*` settings, including `APP_JWT_SECRET` to keep existing sessions valid across server restarts. For listener settings, process environment variables take precedence over `.env` and JSON. The default C++ namespace is `App`. Set a different one with `xmake f --app_namespace=MyApp`.

## Main dependencies

- **Drogon** for HTTP routing and server-rendered views
- **SQLite, libsodium, and jwt-cpp** for account storage, password hashing, and signed sessions
- **CPR, simdjson, and Glaze** for outbound HTTP and JSON handling
- **dotenv-cpp** for loading `.env`
- **Tailwind CSS** for styling and **Catch2** for tests

## Development rules

- Keep `.hpp` files beside their `.cpp` implementations. Use four spaces and put opening braces on the declaration line.
- Keep route handlers small. Delegate persistence and outbound calls, use `co_await` for asynchronous work, `apiOk` and `apiError` for typed JSON replies, and `std::println` for console output.
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

Use `apiOk(*request, item)` for success and
`apiError(*request, status, code, message)` for explicit errors. Errors include
`success: false`, a structured `error`, and request metadata. Throw
`RequestValidationError(message, fields)` to automatically return
`400 VALIDATION_FAILED`. The optional field map stores a list of messages per
field. The response's `X-Request-ID` header matches `meta.request_id`.

### Define and customize the resource

`TodoResource` supplies the public API policy through these DTOs and static functions.

| Policy member | Purpose |
| --- | --- |
| `Response` and `toResponse(const TodoRecord&)` | Return fields clients may read in a Glaze-serializable DTO. |
| `Create`, `parseCreate(const Json::Value&)`, and `makeRecord(Create)` | Validate a POST object, reject unknown fields, and build the new record. |
| `Patch`, `parsePatch(const Json::Value&)`, and `applyPatch(TodoRecord&, Patch)` | Validate writable fields and apply only supplied changes. |
| `addFilter(TodoDao::EqualityFilters&, std::string_view, std::string)` | Map each allowed query key to a typed equality filter and reject unknown keys. |
| Optional `parseId(std::string_view)` | Validate resource-specific ID formats. Generated integer IDs use positive decimal values by default. |

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
