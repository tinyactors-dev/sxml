<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@n != 2" diagnostics="ar el">Not two.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:diagnostics>
    <sch:diagnostic id="ar" xml:lang="ar"><sch:dir value="rtl">ليس اثنين</sch:dir>.</sch:diagnostic>
    <sch:diagnostic id="el" xml:lang="el">Όχι δύο.</sch:diagnostic>
  </sch:diagnostics>
</sch:schema>
