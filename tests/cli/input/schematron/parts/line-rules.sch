<?xml version="1.0" encoding="UTF-8"?>
<sch:pattern xmlns:sch="http://purl.oclc.org/dsdl/schematron" id="lines">
  <sch:rule context="line">
    <sch:assert test="@quantity &gt; 0" id="positive-quantity">A line orders at least one <sch:value-of select="@item"/>.</sch:assert>
  </sch:rule>
</sch:pattern>
