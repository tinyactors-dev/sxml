<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:let name="doubled" value="@n * 2"/>
      <sch:assert test="$doubled &lt; 6" diagnostics="show">Doubled below six.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="show">Doubled: <sch:value-of select="$doubled"/>.</sch:diagnostic>
  </sch:diagnostics>
</sch:schema>
