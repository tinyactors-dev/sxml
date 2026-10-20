(:~ The tree of the TypeScript program for an API description and its schema. :)
import module namespace ts = "https://example.org/typescript" at "typescript.xqm";

declare option output:method "xml";
declare option output:indent "yes";

declare variable $api-file as xs:string external := "api.xml";

let $api := doc($api-file)
let $types := ts:types(doc(resolve-uri($api/*/@types, base-uri($api))))
return <program>{ $types, ts:client($api, $types), ts:server($api, $types) }</program>
