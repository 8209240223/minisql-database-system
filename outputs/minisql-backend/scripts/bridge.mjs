import http from 'node:http';
// Node 内置 HTTP 模块，用来起本地桥接服务。
import { spawn } from 'node:child_process';
// 启动子进程（真正的 C++ 编译器可执行文件）。
import { fileURLToPath } from 'node:url';
// 把 import.meta.url 这种 file:// URL 转成文件系统路径。
const executable=process.env.MINISQL_COMPILER ?? fileURLToPath(new URL('../bin/minisql_compile.exe',import.meta.url));
// 要调用的 C++ 可执行文件：可以用环境变量覆盖，默认取仓库里的 bin/minisql_compile.exe。
const server=http.createServer((req,res)=>{
// 每个请求都走这个处理函数。
  const origin=req.headers.origin;
  // 取请求来源，用于 CORS 白名单判断。
  if(origin && !/^http:\/\/(localhost|127\.0\.0\.1):\d+$/.test(origin)){res.writeHead(403);res.end();return;}
  // 只允许来自本机端口的页面访问；其它来源一律 403，避免被外部网页当成免费编译服务。
  const send=(status,data)=>{if(res.writableEnded)return;res.writeHead(status,{'Content-Type':'application/json; charset=utf-8','Access-Control-Allow-Origin':origin??'http://127.0.0.1:4173','Access-Control-Allow-Headers':'Content-Type','Access-Control-Allow-Methods':'GET, POST, OPTIONS'});res.end(JSON.stringify(data));};
  // 统一的响应工具：如果已经结束就不再写；否则补上 JSON 内容类型与 CORS 头后输出。
  if(req.method==='OPTIONS'){send(200,{});return;}
  // 预检请求直接返回 200，让浏览器继续发真正的请求。
  const capabilities=['lexer','parser','semantic','logicalPlan'];
  // 本桥接当前支持的能力清单（对应 C++ 编译前端已经实现的四个阶段）。
  if(req.method==='GET'&&req.url==='/api/capabilities'){send(200,{engine:'minisql-cpp',schemaVersion:1,capabilities,execution:false,persistence:false,catalogMode:'empty-snapshot'});return;}
  // 能力查询：明确告知"不执行、不持久化、目录是空快照"，避免前端误以为能跑 SQL。
  if(req.method==='GET'&&req.url==='/api/catalog'){send(200,{tables:[],capabilities,catalogMode:'empty-snapshot'});return;}
  // 目录查询：这个桥接不连数据库，所以固定返回空表列表。
  if(req.method==='POST'&&req.url==='/api/execute'){send(501,{error:{message:'C++ 执行器尚未实现；Compile 可返回词法、语法、语义结果和逻辑计划。',code:9001}});return;}
  // 执行接口返回 501（未实现），并告诉用户当前应该改用编译接口。
  if(req.method!=='POST'||req.url!=='/api/compile'){send(404,{error:{message:'Not found'}});return;}
  // 其余请求都不是本服务提供的接口。
  const chunks=[];let length=0;
  // 收集请求体，并累计长度用于限流。
  req.on('data',chunk=>{length+=chunk.length;if(length>8*1024*1024){send(413,{error:{message:'SQL exceeds 8 MiB'}});}else chunks.push(chunk);});
  // 超过 8 MiB 直接回 413，否则继续缓存；这样超大请求不会把内存吃光。
  req.on('end',()=>{
  // 请求体读完后开始处理。
    if(res.writableEnded)return;
    // 前面已经因为超限回过响应，这里就不再继续。
    let sql;try{sql=JSON.parse(Buffer.concat(chunks).toString('utf8')).sql;if(typeof sql!=='string')throw Error();}catch{send(400,{error:{message:'Expected JSON with sql string'}});return;}
    // 解析请求体，要求是 JSON 且 sql 字段是字符串；任何异常都按 400 处理。
    const start=performance.now();const child=spawn(executable,[],{windowsHide:true});let output='';
    // 记下起始时间（用于回报耗时），启动 C++ 进程（隐藏窗口），准备收集它的 stdout。
    const timeout=setTimeout(()=>child.kill(),10000);
    // 10 秒还没结束就杀掉子进程，防止一条慢查询把服务拖住。
    child.on('error',()=>{clearTimeout(timeout);send(503,{error:{message:'C++ compiler executable unavailable'}});});
    // 子进程根本起不来（例如没编译过、路径不对）时回 503。
    child.stdout.setEncoding('utf8');
    // 按 UTF-8 解码，保证中文错误信息不乱码。
    child.stdin.on('error',()=>{});child.stdout.on('data',c=>output+=c);child.stderr.resume();
    // 忽略 stdin 的错误（子进程可能已经退出）；累积标准输出；把标准错误流读掉避免管道阻塞。
    res.on('close',()=>{if(!res.writableEnded)child.kill();});
    // 客户端提前断开时也要杀掉子进程，避免留下孤儿进程。
    child.on('close',()=>{clearTimeout(timeout);try{const data=JSON.parse(output);send(data.success?200:422,{...data,durationMs:performance.now()-start});}catch{send(500,{error:{message:'C++ compiler failed or timed out'}});}});
    // 子进程结束时：清掉超时定时器；把它的输出当 JSON 解析；
    // 成功回 200、编译失败（success 为 false）回 422，并在响应里补上耗时；
    // 输出不是合法 JSON 说明进程崩了或被超时杀掉，回 500。
    child.stdin.end(sql);
    // 把 SQL 写进子进程的标准输入并关闭管道。
  });
});
// 请求处理函数结束。
server.listen(Number(process.env.PORT??8080),'127.0.0.1',()=>console.log(`MiniSQL compiler bridge http://127.0.0.1:${server.address().port}/api`));
// 只监听回环地址（不对外网开放），端口默认 8080，可用 PORT 覆盖；启动后打印实际地址。
