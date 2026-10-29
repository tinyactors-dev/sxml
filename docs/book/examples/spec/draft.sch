<?xml version="1.0" encoding="UTF-8"?>
<!-- Two rules of a Vane draft that its schema, vane.xsd, cannot say. -->
<schema xmlns="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3"
        schematronEdition="2025">
  <title>Rules of a Vane draft</title>

  <pattern id="requirements">
    <rule context="t[bcp14]">
      <assert id="anchored" test="@anchor">A requirement needs an anchor:
        the <value-of select="bcp14[1]"/> in "<value-of
        select="ancestor::section[1]/name"/>" has none.</assert>
    </rule>
  </pattern>

  <pattern id="terms">
    <rule context="dt[@anchor]">
      <assert id="term-used" role="warning" test="@anchor = //xref/@target">The
        term "<value-of select="."/>" is defined, and nothing refers
        to it.</assert>
    </rule>
  </pattern>
</schema>
