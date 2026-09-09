export function generate(seed, count) {
  let state = seed >>> 0;
  const next = () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state >>> 8; };
  const choose = values => values[next() % values.length];
  const number = () => (next() % 19) - 9;
  const expressions = () => choose(['a', 'b', `${number()}`, `(a+${number()})`, `(a-b)*2`, `a/${choose([1, 2, 3, -2])}`, '-(a+b)']);
  const cases = [];
  for (let i = 0; i < count; ++i) {
    const predicates = [];
    const terms = 1 + next() % 4;
    for (let j = 0; j < terms; ++j) {
      const left = expressions(), right = expressions();
      const comparison = `${left}${choose(['=', '!=', '<', '<=', '>', '>='])}${right}`;
      predicates.push(choose([comparison, `NOT (${comparison})`, `(${comparison} OR name='中文')`, `(${comparison} AND 1=1)`]));
    }
    cases.push({ expression: expressions(), predicates, distinct: next() % 2 === 0,
      descending: next() % 2 === 0, limit: next() % 15, offset: next() % 8 });
  }
  return cases;
}

export function render(model) {
  return `SELECT ${model.distinct ? 'DISTINCT ' : ''}id,${model.expression} AS value,name FROM t${model.predicates.length ? ` WHERE ${model.predicates.map(value => `(${value})`).join(' AND ')}` : ''} ORDER BY id ${model.descending ? 'DESC' : 'ASC'},value,name${model.limit === null ? '' : ` LIMIT ${model.limit} OFFSET ${model.offset}`};`;
}

// 仅缩减已知语法模型，避免把参考引擎独有语法误认作缩减后的缺陷。
export function minimize(model, stillFails, budget = 64) {
  let current = structuredClone(model), attempts = 0;
  while (attempts < budget) {
    const candidates = current.predicates.map((_, index) => ({ ...current, predicates: current.predicates.filter((_, i) => i !== index) }));
    if (current.expression !== 'a') candidates.push({ ...current, expression: 'a' });
    if (current.distinct) candidates.push({ ...current, distinct: false });
    if (current.descending) candidates.push({ ...current, descending: false });
    if (current.limit !== null) candidates.push({ ...current, limit: null, offset: 0 });
    let changed = false;
    for (const candidate of candidates) {
      if (++attempts > budget) break;
      if (stillFails(candidate)) { current = candidate; changed = true; break; }
    }
    if (!changed) break;
  }
  return { model: current, attempts: Math.min(attempts, budget) };
}

export const fixture = "CREATE TABLE t(id INT,a INT,b INT,name VARCHAR);" +
  Array.from({ length: 24 }, (_, i) => `INSERT INTO t(id,a,b,name) VALUES(${i % 12},${i % 9 - 4},${i % 5 - 2},'${['中文', "O''Brien", 'a', 'z'][i % 4]}');`).join('');
