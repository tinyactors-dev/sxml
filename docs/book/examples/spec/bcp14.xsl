<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>

  <!-- The key words of BCP 14, from strongest to weakest. -->
  <xsl:variable name="keywords" select="'MUST', 'MUST NOT', 'REQUIRED', 'SHALL',
      'SHALL NOT', 'SHOULD', 'SHOULD NOT', 'RECOMMENDED', 'NOT RECOMMENDED',
      'MAY', 'OPTIONAL'"/>

  <xsl:template name="xsl:initial-template">
    <xsl:text>BCP 14 has {count($keywords)} key words: {string-join($keywords, ', ')}.&#10;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
