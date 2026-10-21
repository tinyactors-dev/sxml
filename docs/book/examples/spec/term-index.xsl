<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>

  <!-- The xrefs, found by the anchor they point to. -->
  <xsl:key name="uses" match="xref" use="@target"/>

  <xsl:template match="/">
    <xsl:for-each-group select="//dt" group-by="substring(., 1, 1)">
      <xsl:sort select="current-grouping-key()"/>
      <xsl:text>{current-grouping-key()}&#10;</xsl:text>
      <xsl:for-each select="current-group()">
        <xsl:sort select="."/>
        <xsl:text>  {.}: </xsl:text>
        <!-- The sections that use the term, each once. -->
        <xsl:for-each select="key('uses', @anchor)/ancestor::section[1]">
          <xsl:if test="position() > 1">, </xsl:if>
          <xsl:number level="multiple" count="section" format="1.1"/>
        </xsl:for-each>
        <xsl:text>&#10;</xsl:text>
      </xsl:for-each>
    </xsl:for-each-group>
  </xsl:template>
</xsl:stylesheet>
