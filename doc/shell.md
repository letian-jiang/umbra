# Shell Design Document

## 1. Overview

Shell 是用户与 UmbraDB 的交互界面——一个基于 `linenoise` (或 `replxx`) 的 REPL，支持 SQL 执行、EXPLAIN、命令历史、行编辑快捷键。

```
$ ./umbra
UmbraDB v0.1 (SF=0.01, 60175 rows loaded)
Type .help for commands, Ctrl-D to exit.

umbra> SELECT COUNT(*) FROM lineitem;
┌──────────┐
│ count(*) │
├──────────┤
│    60175 │
└──────────┘
1 row (0.85 ms)

umbra> .quit
```

---

## 2. REPL Loop

```
main():
    loadData()          // 加载 TPC-H SF=0.01
    printBanner()

    while true:
        line = readline("umbra> ")
        if line == EOF: break
        line = strip(line)
        if line.empty(): continue
        addHistory(line)

        if line starts with '.':
            executeDotCommand(line)
        else:
            executeSQL(line)
```

### 2.1 Multi-line Input

SQL 语句以 `;` 结尾。如果当前行不含 `;`，切换到续行模式（prompt 变为 `   ...> `），继续读取直到遇到 `;`。

```
umbra> SELECT l_returnflag,
   ...>        SUM(l_quantity)
   ...> FROM lineitem
   ...> GROUP BY l_returnflag;
```

```
readStatement():
    buffer = ""
    prompt = "umbra> "
    while true:
        line = readline(prompt)
        if line == EOF: return EOF
        buffer += line + "\n"
        if buffer contains ';':
            return buffer (up to and including ';')
        prompt = "   ...> "
```

---

## 3. Dot Commands

以 `.` 开头的命令，不经过 SQL parser，直接在 shell 层处理。

| Command | Description |
|---------|-------------|
| `.help` | 打印命令列表 |
| `.quit` / `.exit` | 退出 shell |
| `.tables` | 列出所有已加载的表 |
| `.schema <table>` | 打印表的列定义 |
| `.timer on/off` | 开启/关闭每条 SQL 的执行计时 |
| `.backend [interpreter\|flying-start]` | 显示或切换执行后端 |
| `.load <file>` | 加载一个 .tbl 文件 |
| `.explain <level>` | 设置默认 EXPLAIN level（之后每条 SQL 自动 EXPLAIN）|
| `.explain off` | 关闭自动 EXPLAIN |

### 3.1 .help

```
umbra> .help
.help              Show this help
.quit              Exit shell
.tables            List loaded tables
.schema <table>    Show table schema
.timer on|off      Toggle query timing
.backend [name]    Show or set backend (interpreter|flying-start)
.load <file>       Load a .tbl file
.explain <level>   Auto-explain: AST|LOGICAL|PHYSICAL|PIPELINE|IR|ASM|PROFILE|ALL|off
```

### 3.2 .schema

```
umbra> .schema lineitem
Table: lineitem (60175 rows)
  0  l_orderkey       INTEGER
  1  l_partkey        INTEGER
  2  l_suppkey        INTEGER
  3  l_linenumber     INTEGER
  4  l_quantity       DECIMAL(15,2)
  5  l_extendedprice  DECIMAL(15,2)
  6  l_discount       DECIMAL(15,2)
  7  l_tax            DECIMAL(15,2)
  8  l_returnflag     VARCHAR(1)
  9  l_linestatus     VARCHAR(1)
 10  l_shipdate       DATE
 11  l_commitdate     DATE
 12  l_receiptdate    DATE
 13  l_shipinstruct   VARCHAR(25)
 14  l_shipmode       VARCHAR(10)
 15  l_comment        VARCHAR(44)
```

### 3.3 .backend

```
umbra> .backend
Current backend: interpreter

umbra> .backend flying-start
Backend set to: flying-start

umbra> .backend interpreter
Backend set to: interpreter
```

---

## 4. Result Formatting

查询结果以表格形式打印，列宽自适应。

### 4.1 Box-drawing Table

```
┌──────────────┬──────────────┬──────────┐
│ l_returnflag │ l_linestatus │ sum_qty  │
├──────────────┼──────────────┼──────────┤
│ A            │ F            │ 37734107 │
│ N            │ F            │   991417 │
│ N            │ O            │ 74476040 │
│ R            │ F            │ 37719753 │
└──────────────┴──────────────┴──────────┘
4 rows (2.54 ms)
```

### 4.2 格式化规则

| 类型 | 对齐 | 格式 |
|------|------|------|
| INTEGER / BIGINT | 右对齐 | 无千位分隔符 |
| DECIMAL(p,s) | 右对齐 | 固定 s 位小数 |
| DATE | 左对齐 | `YYYY-MM-DD` |
| VARCHAR | 左对齐 | 原样 |

### 4.3 列宽计算

```
for each column:
    width = max(header.length, max(format(row[col]).length for all rows))
    width = min(width, MAX_COL_WIDTH)   // 截断超长值
```

对 Q1 结果集只有 4 行 10 列，全部数据可以一次性 format。

### 4.4 Timing

`.timer on` 后每条 SQL 执行完打印总耗时。`EXPLAIN PROFILE` 打印详细分阶段计时（见 doc/explain.md §2.7）。

```
4 rows (2.54 ms)
```

---

## 5. Line Editing & History

使用 **linenoise**（BSD license, ~1000 行 C, 无依赖）提供行编辑和历史功能。

> 备选：**replxx**（C++ 重写的 linenoise，支持 Unicode 和语法高亮）。MVP 用 linenoise 足够。

### 5.1 Keyboard Shortcuts

linenoise 内置的 Emacs 风格快捷键：

| 快捷键 | 功能 |
|--------|------|
| `Ctrl-A` | 跳到行首 |
| `Ctrl-E` | 跳到行尾 |
| `Ctrl-B` / `←` | 光标左移 |
| `Ctrl-F` / `→` | 光标右移 |
| `Ctrl-D` | 删除当前字符 / 输入为空时退出 |
| `Ctrl-H` / `Backspace` | 删除前一个字符 |
| `Ctrl-K` | 删除从光标到行尾 |
| `Ctrl-U` | 删除整行 |
| `Ctrl-W` | 删除前一个单词 |
| `Ctrl-T` | 交换当前和前一个字符 |
| `Ctrl-L` | 清屏 |
| `Ctrl-P` / `↑` | 上一条历史 |
| `Ctrl-N` / `↓` | 下一条历史 |
| `Ctrl-R` | 反向搜索历史 |
| `Tab` | 自动补全（见 §5.3）|
| `Ctrl-C` | 取消当前输入 |
| `Ctrl-D` (空行) | 退出 shell |

### 5.2 History

```
history_file = ~/.umbra_history
max_history = 1000

startup:
    linenoiseHistoryLoad(history_file)

on each statement:
    linenoiseHistoryAdd(statement)

shutdown:
    linenoiseHistorySave(history_file)
```

历史记录持久化到 `~/.umbra_history`，跨 session 保留。多行 SQL 语句作为一条历史记录保存（换行替换为空格）。

### 5.3 Tab Completion

基于 catalog 的简单补全：

```
completionCallback(input, completions):
    word = lastWordOf(input)
    // 补全表名
    for table in catalog.tables:
        if table.name.startsWith(word):
            completions.add(table.name)
    // 补全列名（如果已经输入了 FROM <table>）
    if currentTable = detectTableFromInput(input):
        for col in currentTable.columns:
            if col.name.startsWith(word):
                completions.add(col.name)
    // 补全关键字
    for kw in [SELECT, FROM, WHERE, GROUP, ORDER, BY, AS,
               SUM, AVG, COUNT, DATE, INTERVAL, EXPLAIN, ...]:
        if kw.lower().startsWith(word):
            completions.add(kw)
```

补全优先级：列名 > 表名 > 关键字。

---

## 6. Error Handling

### 6.1 SQL Errors

```
umbra> SELECT l_unknown FROM lineitem;
Error: column "l_unknown" not found in table "lineitem"

umbra> SELEC * FROM lineitem;
Error: syntax error at position 1, near "SELEC"
```

错误信息包含阶段来源（parser / binder / optimizer / runtime），不打印 stack trace。

### 6.2 Graceful Recovery

任何阶段的错误都 catch 在 REPL 外层，打印错误信息后继续等待下一条输入，不退出 shell。

```
executeSQL(sql):
    try:
        result = compile_and_execute(sql)
        printResult(result)
    catch ParseError as e:
        print "Parse error: " + e.message
    catch BindError as e:
        print "Bind error: " + e.message
    catch RuntimeError as e:
        print "Runtime error: " + e.message
```

---

## 7. Startup

```
$ ./umbra [options]

Options:
  --sf <factor>      TPC-H scale factor (default: 0.01)
  --data <dir>       Directory containing .tbl files (default: ./data)
  --backend <name>   Initial backend: interp|jit (default: jit)
  --help             Show usage
```

启动流程：

```
1. 解析命令行参数
2. 加载 TPC-H 数据（lineitem.tbl）
3. 构建 Catalog
4. 打印 banner:
     UmbraDB v0.1 (SF=0.01, 60175 rows loaded)
     Type .help for commands, Ctrl-D to exit.
5. 进入 REPL loop
```

---

## 8. Non-interactive Mode

支持从 stdin 或文件读取 SQL，用于脚本和测试：

```bash
# 管道输入
echo "SELECT COUNT(*) FROM lineitem;" | ./umbra

# 文件输入（检测 stdin 是否 tty）
./umbra < queries.sql
```

非交互模式下：
- 不打印 banner 和 prompt
- 不加载/保存历史
- 结果输出不带 box-drawing，改为 tab-separated（方便管道处理）
- 错误输出到 stderr

```
$ echo "SELECT COUNT(*) FROM lineitem;" | ./umbra
count(*)
60175
```

---

## 9. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| 语法高亮 | 不实现 | linenoise 不支持，replxx 支持 |
| 多行编辑（上下键在多行间移动）| 不支持，多行通过续行实现 | |
| `.import` / `.export` CSV | 不实现 | |
| `.output` 重定向到文件 | 不实现 | 用 shell 管道代替 |
| 结果分页（`less` 风格）| 不实现 | Q1 只有 4 行 |
| prepared statement / 参数化 | 不支持 | |
| 并发查询 | 不支持 | 单线程 REPL |
| linenoise 行编辑 | 使用简单 stdin 读取代替 | 无快捷键、历史搜索、Tab 补全 |
| 历史持久化 (~/.umbra_history) | 不实现 | 避免依赖 linenoise |
| Tab 自动补全 | 不实现 | 避免依赖 linenoise |
| `.load` / `.explain` dot命令 | 不实现 | MVP 只需 .help/.quit/.tables/.schema/.timer/.backend |
