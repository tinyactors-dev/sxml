<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="v" uri="urn:example:vocabulary"/>
  <sch:pattern>
    <sch:rule context="v:entry">
      <sch:report test="true()">Element <sch:name/>, attribute <sch:name path="@v:code"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
