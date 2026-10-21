import module namespace spec = "urn:example:spec" at "spec.xqm";

declare option output:method "text";

declare variable $anchor as xs:string external := "req-7";

spec:describe(spec:anchor-map(/)($anchor))
