// 全局富悬浮提示：任何带 [title]/[data-tip]/[data-tip-title] 的元素悬停时，
// 显示 VS Code 风格的深色提示框（粗体标题 + 灰色说明行）。
// 用事件委托实现，不需要改任何组件结构；原生 title 在悬停期间被临时屏蔽避免双弹。

let host: HTMLDivElement | null = null;
let source: Element | null = null;
let savedTitle: string | null = null;

const CSS = `
.rich-tip{position:fixed;display:none;z-index:99999;pointer-events:none;max-width:340px;
  background:rgba(37,37,40,.97);border:1px solid #4b4b50;border-radius:5px;padding:7px 10px 8px;
  box-shadow:0 4px 14px rgba(0,0,0,.28);font-family:'Segoe UI',system-ui,sans-serif;font-size:12px;line-height:1.5}
.rich-tip-title{color:#fff;font-weight:600;margin-bottom:2px}
.rich-tip-title:only-child{margin-bottom:0}
.rich-tip-desc{color:#b9b9bf;white-space:pre-line}
.rich-tip-kbd{display:inline-block;padding:0 5px;margin:0 1px;border:1px solid #555;border-bottom-width:2px;border-radius:3px;background:#333;color:#ddd;font-family:Consolas,monospace;font-size:11px}
`;

function ensureHost() {
  if (!host) {
    host = document.createElement('div');
    host.className = 'rich-tip';
    document.body.appendChild(host);
  }
}

function show(target: Element) {
  const title = target.getAttribute('data-tip-title') ?? target.getAttribute('title') ?? '';
  const rawDesc = target.getAttribute('data-tip') ?? '';
  if (!title && !rawDesc) return;
  if (target.hasAttribute('title')) {
    savedTitle = target.getAttribute('title');
    target.setAttribute('title', '');
  }
  ensureHost();
  host!.textContent = '';
  if (title) {
    const head = document.createElement('div');
    head.className = 'rich-tip-title';
    head.textContent = title;
    host!.appendChild(head);
  }
  if (rawDesc) {
    const desc = document.createElement('div');
    desc.className = 'rich-tip-desc';
    desc.innerHTML = rawDesc.replace(/\[kbd\](.+?)\[\/kbd\]/g, '<span class="rich-tip-kbd">$1</span>');
    host!.appendChild(desc);
  }
  host!.style.display = 'block';
  const rect = target.getBoundingClientRect();
  const w = host!.offsetWidth;
  const h = host!.offsetHeight;
  let x = rect.left + rect.width / 2 - w / 2;
  let y = rect.bottom + 8;
  x = Math.max(8, Math.min(x, window.innerWidth - w - 8));
  if (y + h > window.innerHeight - 8) y = rect.top - h - 8;
  if (y < 8) y = 8;
  host!.style.left = `${Math.round(x)}px`;
  host!.style.top = `${Math.round(y)}px`;
  source = target;
}

function hide() {
  if (source?.hasAttribute('title') && savedTitle !== null) {
    source.setAttribute('title', savedTitle);
    savedTitle = null;
  }
  if (host) host.style.display = 'none';
  source = null;
}

export function installRichTooltips() {
  if (document.getElementById('rich-tip-style')) return;
  const style = document.createElement('style');
  style.id = 'rich-tip-style';
  style.textContent = CSS;
  document.head.appendChild(style);
  document.addEventListener('mouseover', event => {
    const target = (event.target as Element | null)?.closest?.('[title],[data-tip],[data-tip-title]') ?? null;
    if (target === source) return;
    hide();
    if (target) show(target);
  }, true);
  document.addEventListener('mouseout', event => {
    if (source && !source.contains(event.relatedTarget as Node)) hide();
  }, true);
  window.addEventListener('scroll', hide, true);
  window.addEventListener('blur', hide);
}
