// SQL 对象引用提取器。CLI、HTTP bridge 和审计统一使用这一份保守扫描逻辑。
// 它跳过注释与字符串，排除 CTE 名称和派生表别名，避免鉴权出现路径差异。

export function firstKeyword(sql) {
  return String(sql ?? '').replace(/^\s+|\s+$/g, '')
    .match(/^(?:\/\*[\s\S]*?\*\/\s*|--[^\r\n]*\r?\n\s*)*([a-zA-Z]+)/)?.[1]?.toUpperCase() ?? '';
}

function sqlWords(sql) {
  const words = [];
  const source = String(sql ?? '');
  for (let index = 0; index < source.length;) {
    const character = source[index];
    if (/\s/.test(character)) { ++index; continue; }
    if (source.startsWith('--', index)) {
      index = source.indexOf('\n', index + 2);
      if (index < 0) break;
      continue;
    }
    if (source.startsWith('/*', index)) {
      const end = source.indexOf('*/', index + 2);
      index = end < 0 ? source.length : end + 2;
      continue;
    }
    if (character === "'") {
      ++index;
      while (index < source.length) {
        if (source[index] !== "'") { ++index; continue; }
        if (source[index + 1] === "'") { index += 2; continue; }
        ++index;
        break;
      }
      continue;
    }
    const identifier = source.slice(index).match(/^[A-Za-z_][A-Za-z0-9_]*/);
    if (identifier) {
      words.push(identifier[0].toLowerCase());
      index += identifier[0].length;
      continue;
    }
    words.push(character);
    ++index;
  }
  return words;
}

export function tableReferences(sql, keyword) {
  const words = sqlWords(sql);
  const tables = [];
  const ctes = new Set();
  const identifier = value => /^[a-z_][a-z0-9_]*$/.test(value ?? '');
  const add = value => { if (identifier(value) && !ctes.has(value) && !tables.includes(value)) tables.push(value); };
  const addAfter = (index, allowParenthesized = true) => {
    let cursor = index + 1;
    if (allowParenthesized && words[cursor] === '(') return;
    if (words[cursor] === 'lateral') ++cursor;
    add(words[cursor]);
  };
  if (words[0] === 'with') {
    let cursor = words[1] === 'recursive' ? 2 : 1;
    for (;;) {
      if (!identifier(words[cursor])) break;
      ctes.add(words[cursor++]);
      if (words[cursor] !== 'as' || words[cursor + 1] !== '(') break;
      cursor += 2;
      let depth = 1;
      while (cursor < words.length && depth) {
        if (words[cursor] === '(') ++depth;
        else if (words[cursor] === ')') --depth;
        ++cursor;
      }
      if (words[cursor] !== ',') break;
      ++cursor;
    }
  }
  const normalizedKeyword = String(keyword ?? '').toLowerCase();
  const effectiveKeyword = normalizedKeyword === 'explain'
    ? (words.find(word => word === 'select' || word === 'insert' || word === 'update' || word === 'delete') ?? normalizedKeyword)
    : normalizedKeyword;
  for (let index = 0; index < words.length; ++index) {
    const word = words[index];
    if (word === 'from' || word === 'join' || word === 'into' || word === 'update' || word === 'references') addAfter(index);
    if (effectiveKeyword === 'drop' && (word === 'table' || word === 'on')) addAfter(index, false);
    if (effectiveKeyword === 'create' && (word === 'table' || word === 'on')) addAfter(index, false);
  }
  return tables;
}
