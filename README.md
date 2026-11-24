# App Template

A C++26 web application starter built with Drogon and Xmake. The demo includes a server-rendered page, cookie-based sign-in, SQLite user storage, file uploads and downloads, and an upstream JSON endpoint.

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
    auto page = co_await tasks.find(
        20, 0,
        select<&TaskRecord::note, &TaskRecord::related_project_id>(),
        orderBy<&TaskRecord::title>(),
        where<&TaskRecord::project_id>(website_id));

    if (!page.empty()) {
        auto& task = page.front();
        if (task.isLoaded<&TaskRecord::note>() && task.note) {
            std::println("Note: {}", *task.note);
        }
        task.status.value = TaskStatus::done;
        co_await tasks.update(task);
    }

    // Count matches across all pages.
    const auto total = co_await tasks.count(
        where<&TaskRecord::project_id>(in({website_id, docs_id})),
        where<&TaskRecord::title>(contains("docs")));
    std::println("Matching tasks: {}", total);

    // Read by ID and explicitly fetch the related project.
    const auto task = co_await tasks.findById(
        task_id, withForeign<&TaskRecord::related_project_id>(projects));
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
| `find(...)` | Defaults to 100 rows at offset 0. Returns the key and required fields, sorted by newest update when timestamps are enabled and by primary key otherwise. Use `select<...>()` or `selectAll()` for nullable fields, and `orderBy<Member>(SortDirection::desc)` for descending order. |
| `findById(id)` | Returns an optional full record. Foreign fields contain keys. Pass `withForeign<Member>(target_dao)` to load selected related records, or call `target_dao.loadForeign(field)` later. |
| `update(record)` | Updates by primary key and preserves fields that `find()` omitted. A loaded nullable field set to `std::nullopt` clears the value. |
| `deleteById(id)` | Deletes by primary key and reports whether a row was deleted. |
| `count(where<...>(...))` | Counts all matching rows and requires at least one predicate. |
| `executeSql(sql, args...)` | Runs custom, backend-specific SQL with bound arguments. |

`where<Member>(value)` means equality. Use `eq`, `gt`, `gte`, `lt`, `lte`, `in`, or `contains` to name an operator. Separate filters combine with `AND`. `contains` matches literal, case-sensitive text. `negate(where<...>(...))` includes rows with `NULL` when the original filter excludes them.

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
- Keep route handlers small. Delegate persistence and outbound calls, use `co_await` for asynchronous work, `jsonResponse` for typed JSON replies, and `std::println` for console output.
- After editing a view or Tailwind source, run `npm ci` and `npm run build:css` with Node.js installed. Include the regenerated `public/demo.css` in the change.
- Keep `.env`, local databases, and runtime uploads out of commits. See [AGENTS.md](AGENTS.md) for the full repository guidelines.
