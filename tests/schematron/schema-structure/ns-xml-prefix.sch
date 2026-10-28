<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="xml" uri="http://www.w3.org/XML/1998/namespace"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:assert test="@xml:lang = 'en'">English only.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
