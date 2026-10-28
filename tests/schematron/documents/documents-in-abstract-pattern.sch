<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern abstract="true" id="remote" documents="$file">
    <sch:rule context="item">
      <sch:report test="@n = 5">Remote five.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern is-a="remote">
    <sch:param name="file" value="'documents-in-abstract-pattern/sub.xml'"/>
  </sch:pattern>
</sch:schema>
