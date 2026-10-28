<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:ns prefix="f" uri="urn:example:functions"/>
  <xsl:include xmlns:xsl="http://www.w3.org/1999/XSL/Transform" href="binding-xslt3-include/functions.xsl"/>
  <sch:pattern>
    <sch:rule context="item">
      <sch:report test="f:is-odd(@n)">Odd.</sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>
