(: The requirements of the draft, section by section. :)
declare option output:indent "yes";

<requirements draft="{ /rfc/@docName }">{
  for $section in //section[t/@anchor]
  return
    <section anchor="{ $section/@anchor }" name="{ $section/name }">{
      for $requirement in $section/t[@anchor]
      return <requirement anchor="{ $requirement/@anchor }">{
        string($requirement/bcp14)
      }</requirement>
    }</section>
}</requirements>
