<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="größe">
      <sch:assert test="@wert &lt; 10"><sch:name/> zu groß: <sch:value-of select="@wert"/>.</sch:assert>
    </sch:rule>
    <sch:rule context="𝒳">
      <sch:report test="string-length(@wert) = 1"><sch:name/> hat ein Zeichen: <sch:value-of select="@wert"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
