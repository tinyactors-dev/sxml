<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3" icon="s.png" see="http://example.org/s" fpi="-//EXAMPLE//S">
  <sch:pattern icon="p.png" see="http://example.org/p" fpi="-//EXAMPLE//P">
    <sch:rule context="item" icon="r.png" see="http://example.org/r" xml:lang="en">
      <sch:report test="@n = 2" icon="a.png" see="http://example.org/a" fpi="-//EXAMPLE//A">Item two.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
