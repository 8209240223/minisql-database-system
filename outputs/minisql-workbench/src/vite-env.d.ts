/// <reference types="vite/client" />
declare module 'node-sql-parser/build/sqlite' {
  export class Parser { astify(sql: string): unknown }
}
