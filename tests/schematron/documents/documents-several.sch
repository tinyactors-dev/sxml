<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern documents="('documents-several/a.xml', 'documents-several/b.xml')">
    <sch:rule context="/*">
      <sch:report test="true()">Root <sch:name/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
