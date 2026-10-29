<?xml version="1.0" encoding="UTF-8"?>
<!-- A pattern over another document (5.4.12): its findings are in it. -->
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:let name="ordered" value="//line/@item ! string()"/>
  <sch:pattern id="lines">
    <sch:rule context="line">
      <sch:assert id="positive" test="xs:integer(@quantity) gt 0">A line orders something.</sch:assert>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="stock" documents="'stock.xml'">
    <sch:rule context="item[@name = $ordered]">
      <sch:assert id="in-stock" test="xs:integer(@count) gt 0">The <sch:value-of select="@name"/> ordered is out of stock.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
