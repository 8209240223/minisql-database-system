import { createInterface } from 'node:readline';
console.log(JSON.stringify({type:'ready',protocolVersion:1}));
createInterface({input:process.stdin}).on('line',line=>{
  const request=JSON.parse(line);
  const response=JSON.stringify({id:request.id,success:true});
  switch(request.operation){
    case 'split':
      process.stdout.write(response.slice(0,8));
      setTimeout(()=>process.stdout.write(response.slice(8)+'\n'),10);break;
    case 'mismatch': console.log(JSON.stringify({id:'wrong',success:true}));break;
    case 'malformed': console.log('{bad}');break;
    case 'oversize': console.log(JSON.stringify({id:request.id,success:true,padding:'x'.repeat(4096)}));break;
    case 'hang': break;
    case 'exit': process.exit(17);break;
    case 'close': process.stdout.write(response+'\n',()=>process.exit(0));break;
    default: console.log(response);
  }
});
