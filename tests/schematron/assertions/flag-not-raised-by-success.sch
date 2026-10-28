<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert flag="big" test="@n &lt; 9">Small.</sch:assert>
      <sch:report flag="huge" test="@n &gt; 9">Huge.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
