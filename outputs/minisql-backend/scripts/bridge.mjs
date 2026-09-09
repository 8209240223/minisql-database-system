import http from 'node:http';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const executable=process.env.MINISQL_COMPILER ?? fileURLToPath(new URL('../bin/minisql_compile.exe',import.meta.url));
const server=http.createServer((req,res)=>{
  const origin=req.headers.origin;
  if(origin && !/^http:\/\/(localhost|127\.0\.0\.1):\d+$/.test(origin)){res.writeHead(403);res.end();return;}
  const send=(status,data)=>{if(res.writableEnded)return;res.writeHead(status,{'Content-Type':'application/json; charset=utf-8','Access-Control-Allow-Origin':origin??'http://127.0.0.1:4173','Access-Control-Allow-Headers':'Content-Type','Access-Control-Allow-Methods':'GET, POST, OPTIONS'});res.end(JSON.stringify(data));};
  if(req.method==='OPTIONS'){send(200,{});return;}
  const capabilities=['lexer','parser','semantic','logicalPlan'];
  if(req.method==='GET'&&req.url==='/api/capabilities'){send(200,{engine:'minisql-cpp',schemaVersion:1,capabilities,execution:false,persistence:false,catalogMode:'empty-snapshot'});return;}
  if(req.method==='GET'&&req.url==='/api/catalog'){send(200,{tables:[],capabilities,catalogMode:'empty-snapshot'});return;}
  if(req.method==='POST'&&req.url==='/api/execute'){send(501,{error:{message:'C++ 执行器尚未实现；Compile 可返回词法、语法、语义结果和逻辑计划。',code:9001}});return;}
  if(req.method!=='POST'||req.url!=='/api/compile'){send(404,{error:{message:'Not found'}});return;}
  const chunks=[];let length=0;
  req.on('data',chunk=>{length+=chunk.length;if(length>8*1024*1024){send(413,{error:{message:'SQL exceeds 8 MiB'}});}else chunks.push(chunk);});
  req.on('end',()=>{
    if(res.writableEnded)return;
    let sql;try{sql=JSON.parse(Buffer.concat(chunks).toString('utf8')).sql;if(typeof sql!=='string')throw Error();}catch{send(400,{error:{message:'Expected JSON with sql string'}});return;}
    const start=performance.now();const child=spawn(executable,[],{windowsHide:true});let output='';
    const timeout=setTimeout(()=>child.kill(),10000);
    child.on('error',()=>{clearTimeout(timeout);send(503,{error:{message:'C++ compiler executable unavailable'}});});
    child.stdout.setEncoding('utf8');
    child.stdin.on('error',()=>{});child.stdout.on('data',c=>output+=c);child.stderr.resume();
    res.on('close',()=>{if(!res.writableEnded)child.kill();});
    child.on('close',()=>{clearTimeout(timeout);try{const data=JSON.parse(output);send(data.success?200:422,{...data,durationMs:performance.now()-start});}catch{send(500,{error:{message:'C++ compiler failed or timed out'}});}});
    child.stdin.end(sql);
  });
});
server.listen(Number(process.env.PORT??8080),'127.0.0.1',()=>console.log(`MiniSQL compiler bridge http://127.0.0.1:${server.address().port}/api`));
