<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="remote">
    <sch:rule context="$element">
      <sch:report test="true()">Remote <sch:name/>.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="remote" documents="'documents-abstract-parameter/sub.xml'">
    <sch:param name="element" value="item[@n = 6]"/>
  </sch:pattern>
</sch:schema>
