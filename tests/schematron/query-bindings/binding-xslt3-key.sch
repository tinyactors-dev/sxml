<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <xsl:key xmlns:xsl="http://www.w3.org/1999/XSL/Transform" name="by-id" match="product" use="@id"/>
  <sch:pattern>
    <sch:rule context="line">
      <sch:assert test="key('by-id', @product)">The line names a known product.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
