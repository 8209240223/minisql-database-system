// SQL 对象引用提取器。CLI、HTTP bridge 和审计统一使用这一份保守扫描逻辑。
// 它跳过注释与字符串，排除 CTE 名称和派生表别名，避免鉴权出现路径差异。

export function firstKeyword(sql) {
// 取出 SQL 的第一个关键字（大写）；前导的块注释与行注释都会被跳过。
  return String(sql ?? '').replace(/^\s+|\s+$/g, '')
    // 先去掉首尾空白，避免正则要从空白开始匹配。
    .match(/^(?:\/\*[\s\S]*?\*\/\s*|--[^\r\n]*\r?\n\s*)*([a-zA-Z]+)/)?.[1]?.toUpperCase() ?? '';
    // 用一条正则跳过任意多个"块注释 + 空白"或"行注释 + 换行 + 空白"，再抓住第一个连续的字母串；
    // 抓到就转大写，抓不到（例如整段都是注释）返回空串。
}

function sqlWords(sql) {
// 把 SQL 切成一串"词"：跳过空白与注释，字符串字面量整体丢弃，标识符转小写。
  const words = [];
  // 结果词表。
  const source = String(sql ?? '');
  // 统一转成字符串，容忍 null/undefined。
  for (let index = 0; index < source.length;) {
  // 逐字符扫描。
    const character = source[index];
    // 当前字符。
    if (/\s/.test(character)) { ++index; continue; }
    // 空白直接跳过。
    if (source.startsWith('--', index)) {
    // 行注释。
      index = source.indexOf('\n', index + 2);
      // 跳到行尾。
      if (index < 0) break;
      // 没有换行说明注释直到文件末尾，扫描到此结束。
      continue;
      // 注释不产生词。
    }
    // 行注释分支结束。
    if (source.startsWith('/*', index)) {
    // 块注释。
      const end = source.indexOf('*/', index + 2);
      // 找结束标记。
      index = end < 0 ? source.length : end + 2;
      // 没找到就直接跳到末尾（畸形注释当作吃掉后面全部内容）。
      continue;
      // 注释不产生词。
    }
    // 块注释分支结束。
    if (character === "'") {
    // 字符串字面量：整体跳过，不产生词。
      ++index;
      // 跳过起始引号。
      while (index < source.length) {
      // 直到闭合或到末尾。
        if (source[index] !== "'") { ++index; continue; }
        // 普通字符继续前进。
        if (source[index + 1] === "'") { index += 2; continue; }
        // 连续两个单引号是转义，整体跳过。
        ++index;
        // 否则是结束引号。
        break;
        // 跳出。
      }
      // 字符串扫描结束。
      continue;
      // 字符串不产生词。
    }
    // 字符串分支结束。
    const identifier = source.slice(index).match(/^[A-Za-z_][A-Za-z0-9_]*/);
    // 尝试从当前位置匹配一个标识符（字母或下划线开头）。
    if (identifier) {
    // 匹配成功。
      words.push(identifier[0].toLowerCase());
      // 转小写后收进词表（对象名比较要大小写不敏感）。
      index += identifier[0].length;
      // 游标跳过整个标识符。
      continue;
      // 继续扫描。
    }
    // 标识符分支结束。
    words.push(character);
    // 其它字符（括号、逗号、点号、运算符）各自作为单独一个"词"保留，
    // 后面识别结构时要靠它们定位。
    ++index;
    // 前进一格。
  }
  return words;
  // 返回词表。
}

export function tableReferences(sql, keyword) {
// 从 SQL 文本里保守地抽取"被访问的表名"，供鉴权用。
  const words = sqlWords(sql);
  // 先切词。
  const tables = [];
  // 抽出的表名。
  const ctes = new Set();
  // WITH 子句定义的 CTE 名字，这些是作用域名不是真实对象，必须排除。
  const identifier = value => /^[a-z_][a-z0-9_]*$/.test(value ?? '');
  // 判断一个词是否像合法标识符（词表里已经统一小写）。
  const add = value => { if (identifier(value) && !ctes.has(value) && !tables.includes(value)) tables.push(value); };
  // 加入结果：必须是标识符、不是 CTE、而且还没出现过。
  const addAfter = (index, allowParenthesized = true) => {
  // 把紧跟某个关键字后面的词当作表名。
    let cursor = index + 1;
    // 从关键字的下一个词开始。
    if (allowParenthesized && words[cursor] === '(') return;
    // 允许括号时：FROM (SELECT ...) 是派生表，本身不是对象。
    if (words[cursor] === 'lateral') ++cursor;
    // LATERAL 只是修饰词，跳过再看真正的表名。
    add(words[cursor]);
    // 收进结果。
  };
  if (words[0] === 'with') {
  // 语句以 WITH 开头，先收集 CTE 名字。
    let cursor = words[1] === 'recursive' ? 2 : 1;
    // WITH RECURSIVE 时从第 3 个词开始，否则从第 2 个词开始。
    for (;;) {
    // 逐个 CTE 处理。
      if (!identifier(words[cursor])) break;
      // 当前词不是标识符，说明 CTE 列表结束。
      ctes.add(words[cursor++]);
      // 记下这个 CTE 名字并前移游标。
      if (words[cursor] === '(') {
      // CTE 名字后面可能跟列名清单（形如 cte(a, b) AS ...）。
        let columnDepth = 1;
        // 括号深度从 1 开始。
        ++cursor;
        // 跳过左括号。
        while (cursor < words.length && columnDepth) {
        // 一直找到配对的右括号。
          if (words[cursor] === '(') ++columnDepth;
          // 左括号加深。
          else if (words[cursor] === ')') --columnDepth;
          // 右括号变浅。
          ++cursor;
          // 前移。
        }
        // 列名清单跳过结束。
      }
      // 列名清单处理结束。
      if (words[cursor] !== 'as' || words[cursor + 1] !== '(') break;
      // CTE 定义必须是 AS ( ... )，否则结束扫描。
      cursor += 2;
      // 跳过 AS 与左括号。
      let depth = 1;
      // 括号深度从 1 开始。
      while (cursor < words.length && depth) {
      // 找到 CTE 主体的配对右括号。
        if (words[cursor] === '(') ++depth;
        // 左括号加深。
        else if (words[cursor] === ')') --depth;
        // 右括号变浅。
        ++cursor;
        // 前移。
      }
      // 主体跳过结束。
      if (words[cursor] !== ',') break;
      // 后面没有逗号说明 CTE 列表结束。
      ++cursor;
      // 跳过逗号，处理下一个 CTE。
    }
    // CTE 收集结束。
  }
  const normalizedKeyword = String(keyword ?? '').toLowerCase();
  // 规范化传入的关键字。
  const effectiveKeyword = normalizedKeyword === 'explain'
  // EXPLAIN 后面才是真正的语句关键字，需要单独找出来。
    ? (words.find(word => word === 'select' || word === 'insert' || word === 'update' || word === 'delete') ?? normalizedKeyword)
    // 找到四种数据语句任一就采用它；找不到就退回原关键字。
    : normalizedKeyword;
    // 其它情况直接用传入的关键字。
  for (let index = 0; index < words.length; ++index) {
  // 从头遍历词序列找对象。
    const word = words[index];
    // 当前词。
    if (word === 'from' || word === 'join' || word === 'into' || word === 'update' || word === 'references') addAfter(index);
    // FROM/JOIN 是查询来源，INTO 是插入目标，UPDATE 是更新目标，REFERENCES 是外键父表。
    if (effectiveKeyword === 'drop' && (word === 'table' || word === 'on')) addAfter(index, false);
    // DROP TABLE 后面的表；DROP INDEX ... ON 后面的表。
    if (effectiveKeyword === 'create' && (word === 'table' || word === 'on')) addAfter(index, false);
    // CREATE TABLE 与 CREATE INDEX ... ON 后面的表。
  }
  return tables;
  // 返回抽出的表名。
}
