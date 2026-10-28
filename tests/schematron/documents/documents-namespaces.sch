<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="e" uri="urn:example:e"/>
  <sch:pattern documents="'documents-namespaces/sub.xml'">
    <sch:rule context="e:item">
      <sch:report test="true()">Namespaced item.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
