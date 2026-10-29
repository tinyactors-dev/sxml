xquery version "4.0";

(:~
 : Write every article file again in canonical form (spec:put). The import
 : uses it on the files it writes; nothing else needs it.
 :)
import module namespace spec = "urn:sxml:spec" at "spec/lib/spec.xqm";

for $article in collection(spec:uri("spec/articles") || "?recurse=yes")
return spec:put($article/*, document-uri($article))
