<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- The default collection is the directory the command line names. -->
  <xsl:output method="text"/>
  <xsl:template name="xsl:initial-template">
    <xsl:for-each select="collection()/branch">
      <xsl:value-of select="@name, count(loan)"/>
      <xsl:text>&#10;</xsl:text>
    </xsl:for-each>
  </xsl:template>
</xsl:stylesheet>
